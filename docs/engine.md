# The engine: PipeWire inside ear

ear does not connect to a PipeWire daemon: it **is** the PipeWire instance. `Engine`
(`src/pipewire/engine.cpp`) makes PipeWire's main loop and context in ear's own process and
connects to that context itself (`pw_context_connect_self`); every adapter in `src/pipewire/` works
through that one connection. There is no daemon, no socket, no runtime directory, no D-Bus and no
session manager. *Why:* the owner wanted PipeWire to "handle low-level audio concerns so that `ear`
can focus on the graph and UX", inside one container.

Every claim here was checked against **PipeWire 1.4.2** (Debian 13 "trixie", the build image) by
the tests in `src/pipewire/tests/`. Source citations are to PipeWire's tree at that version unless
marked otherwise; read them again at a PipeWire bump ([below](#bumping-pipewire)). The
`pipewire-engine` skill (`.claude/skills/pipewire-engine/SKILL.md`) is the working guide for
changing this code.

## What the engine is made of

| Piece | Where | What |
|---|---|---|
| Main loop, context, own connection | `Engine::create` | One process, one loop; every core object and adapter runs on the main loop's thread |
| `engine.conf` | `src/pipewire/engine.conf`, copied beside `out/ear` | Read by `pw_context_new`; loads the modules and the internal clock |
| Clock settings | `contextProperties()` in `engine.cpp` | The graph frequency and block size, fixed at boot |
| Port blocks | `port_blocks.cpp` | One `pw_filter` per node; `drive`, fades, levels, `GraphCycle`, latency |
| Level mailbox | `level_mailbox.hpp` | One triple buffer per block |
| Live ports | `live_graph.cpp` | Registry globals joined into node name, port name, direction, port id |
| Links | `links.cpp`, `link_proxy.hpp` | `link-factory` links by port global id |
| Engine companions | `engine_companions.cpp` (`joinEngineCompanion`) | The one lifecycle of every engine node a node's configuration asks for |
| Sound cards | `sound_cards.cpp` | One ALSA adapter node per card and direction |
| Module-run converters | `module_converter_body.hpp`, `converter_modules.hpp`, `module_converters.hpp` | The engine companion a PipeWire module runs: loaded, found by name, linked, its latency carried, its life judged ([below](#module-run-converters)) |
| RTP receivers | `rtp_receivers.cpp` | The first module-run converter: `libpipewire-module-rtp-source`, one per `rtp` node |
| Sinks and handoffs | `handoffs.cpp` | `ThreadSinks` (the sinks: a ring the block taps and a thread that drains it, [below](#sinks)) and `ThreadHandoffs` (the legacy handoff `sendspin` uses) |
| Sources | `sources.cpp` | `EventSources`: core's `SourceRing` with the writer's wait and the clock wake's eventfd ([below](#sources)) |
| The engine's log | `engine_log.cpp` | ear's own `pw_log` and alsa-lib's error handler ([output-records.md](output-records.md#how-output-is-written)), both `noexcept` and registered through the callback boundary as realtime functions |
| The wedge mark | `data_loop_wedge.hpp` | Once the data loop is judged wedged, the adapters stop calling into it |
| Main-loop executor | `main_loop_executor.cpp` | Work posted from any thread runs on the main loop |
| The callback boundary | `callback_boundary.hpp`, `faults.hpp`, `no_raw_callbacks.hpp` | Every C function ear gives PipeWire or alsa-lib, with no exception: `guarded<>` (caught, reported to the engine's `Faults`), `realtime<>` on the data loop (a `DataLoop` takes nothing else), `realtimeFunction<>` for the log and alsa-lib's handler; every raw registration poisoned after it in every source of the package ([architecture.md](architecture.md#the-callback-boundary)) |
| The internal clock's cuts | `driver_cuts.hpp` | `DriverCuts`: a realtime listener on the dummy driver's `start` and `incomplete` events, the record every block numbers its cycles by and reads its cuts from ([below](#graphcycle)) |
| Shared names | `engine_names.hpp` | The graph group's name, a card's driver priority, the internal clock's node and clock names |
| The property table | `engine_properties.hpp`, `engine_property_set.cpp` | Every PipeWire property ear sets, in `engine.conf` or in code, and every one it never sets; a key is a `PropertyKey` only the table makes, and only `engine_property_set` writes properties ([below](#properties-ear-sets)) |

`main.cpp` passes `<directory of /proc/self/exe>/engine.conf`; the tests pass the source tree's
file. The test `ear_engine_conf_beside_ear` fails if the copy beside the binary differs.

## Properties ear sets

Every PipeWire property ear sets, in `engine.conf` or in code, is one row of
`src/pipewire/engine_properties.hpp`, and this table is the home of each one's reason and of what
it couples to. Where says which object carries it and which code builds it: `engine.conf` (its
`context.properties`), `context` (`contextProperties()` in `engine.cpp`, `pw_context_new`'s own
properties), `internal clock` (`engine.conf`'s `context.objects`), `block` and `block port`
(`port_blocks.cpp`), `card` (`sound_cards.cpp`), `card seam` (what `AlsaSoundCards`' test seam
adds: `clocklessCardProperties()` alone), `RTP receiver` (`libpipewire-module-rtp-source`'s
arguments, `rtp_receivers.cpp`), `RTP stream` (its `stream.props`), `route link` (`links.cpp`)
and `companion link` (`engine_companions.cpp`). The table is checked four ways:

- **By type, at compile time.** A key carries its home: `property<Home::h>("key")` returns a
  `PropertyKey<h>` and is the only way to make one from a string; it does not compile for a key the
  table does not list for that home, nor for one it forbids there. The only code that writes
  PipeWire properties, makes a core object from them or loads a module with arguments is
  `engine_property_set.{hpp,cpp}`: `PropertySet<h>` (an object's properties, set only with
  `PropertyKey<h>`, released to the PipeWire call that takes them), `createObject` (which takes only
  a `PropertySet`), `ModuleArguments<h>` and `loadModule` (an RTP receiver's arguments, whose
  `stream.props` alone nests `ModuleArguments<Home::rtpStream>`). So a string, a `const char*` or
  a key of another home where a key is wanted does not compile
  (`ear_engine_properties_compile_fail_*`). The one key made at run time is the card seam's:
  `seamChecked(key)` refuses a key the table forbids on a card or sets on every card and returns a
  `PropertyKey<Home::cardSeam>`, which only a card's `PropertySet` takes; its text is an element of
  the `AlsaSoundCards`' own extra properties (a temporary does not compile), and
  `pw_properties_set` copies it. A module argument's value that would open another key (whitespace
  outside quotes, an array or an object, or nothing) throws `std::logic_error`. The table's own
  `static_assert`s refuse a key listed twice, a row [ear never sets](#properties-ear-never-sets), a
  key in both `engine.conf` and `context` (the file would overwrite `Engine`'s) and a card seam key
  that a card already sets. What the type does not cover is which row a builder picks for a value:
  that is the live check's.
- **In the source.** The property guard (`ear_properties_from_the_table`,
  `test/pipewire/property_guard.cmake`) keeps `engine_property_set` the only writer: under `src/`
  outside it and tests, it refuses any whole-word mention (a call, a function pointer) of
  PipeWire's property writers (every `pw_properties_new*`, `_set*`, `_update*`, `_add*`, every
  `pw_*_update_properties`), `pw_core_create_object`, `pw_context_load_module` and
  `pw_impl_module_load*`, `SPA_DICT_ITEM*` and `SPA_DICT_INIT*`; `spa_dict_item` anywhere but as a
  `const spa_dict_item*`; and a `spa_dict` made in place (a variable with an initializer, a
  temporary, a compound literal). It reads each line with its literals emptied before its comments
  are removed. It does not read raw string literals (`src/` has none) or code built from macros of
  ear's own.
- **Against the live engine.** "Every property ear sets is held by PipeWire as the property table
  says" makes one of every object and reads each row's value as PipeWire holds it on that object,
  through the context's own objects, so a PipeWire that drops or overrides one fails. Links are
  found by their nodes' ids, and a port row is the name of the port PipeWire's held id is on that
  node. The RTP receiver's rows are read from its node (`source.ip` and `source.port` as
  `rtp.source.ip` and `rtp.source.port`) and from the module's arguments as PipeWire parsed them;
  `remote.name` and `stream.props` are held only there. The RTP stream's
  `adapter.auto-port-config` is consumed by the stream's own adapter and never reaches the node:
  it is read in the arguments, and its effect is checked as the node's exact ports (one
  `receive_AUX<n>` per channel, no monitor or control port). No object holds a key
  [ear never sets](#properties-ear-never-sets) on its home, except `node.driver` on a card, which
  the ALSA node publishes itself. "engine.conf sets exactly the property table's engine.conf and
  internal clock rows, and nothing it forbids" reads the file with PipeWire's own parser: it holds
  `context.properties`, `context.spa-libs`, `context.modules` and `context.objects` and no other
  section (libpipewire reads properties from `filter.properties`, `stream.properties`,
  `node.rules` and more), and no module is loaded with arguments.
- **Each coupling pinned.** Every row names the test that pins it ("Pinned by"); "Every row of the
  property table is pinned by a test that exists" fails for a name no test has.
- **Against this page.** "docs/engine.md lists the property table's rows and forbidden keys, each
  row's why under a heading that exists" fails when the rows below (where, key, value, pinned by)
  and the code's differ, when a why does not link its row's heading, and when the next section's
  keys and the code's forbidden list differ.

A value in backticks is the value as set, exactly; prose says how it is computed.

The table holds properties, not params. What ear sends as a param is out of its scope: a card's
port configuration (`SPA_PARAM_PortConfig`, [sound cards](#sound-cards)), the counterpart of the
RTP stream's `adapter.auto-port-config`; a block's latency ([latency](#latency-per-bank-pair));
and a filter's connect flags.

| Where | Key | Value | Why | Couples to | Pinned by |
|---|---|---|---|---|---|
| engine.conf | `core.daemon` | `false` | No socket and no runtime directory: ear's own connection is the only client. Serving the socket is parked ([here](#properties-ear-sets)) | no socket and no runtime directory | "The engine runs with no daemon, runtime directory or D-Bus, and an unlinked sink block processes zeros" |
| engine.conf | `core.name` | `ear` | Names the context; nothing reads it ([here](#properties-ear-sets)) | nothing | "Every property ear sets is held by PipeWire as the property table says" |
| engine.conf | `support.dbus` | `false` | There is no D-Bus in the container (`context.c:315`) ([here](#properties-ear-sets)) | no D-Bus connection | "The engine runs with no daemon, runtime directory or D-Bus, and an unlinked sink block processes zeros" |
| engine.conf | `context.num-data-loops` | `1` | 1.4.2's default (`context.c`, `DEFAULT_DATA_LOOPS`), pinned because the node framework relies on it: a block and the engine nodes beside it see one `clock.position` per cycle, and a non-blocking invoke on that loop runs between any two of their cycles. A bump that changes the default must keep it ([the one data loop](#the-one-data-loop-and-invokes)) | one data loop runs every engine node, so one `clock.position` a cycle and invokes between cycles | "S3: the engine has one data loop, context.num-data-loops = 1, and blocks, a sound card and an RTP receiver all run on it" |
| context | `config.name` | the engine.conf path EngineSettings names | `main.cpp` passes `<directory of /proc/self/exe>/engine.conf`, the tests the source tree's; `Engine` does no lookup of its own. The file's `context.properties` overwrite `pw_context_new`'s own (`context.c:245`, `conf.c:986-990`), so the two homes share no key ([here](#properties-ear-sets)) | the file `pw_context_new` reads; its `context.properties` overwrite these | "Every property ear sets is held by PipeWire as the property table says" |
| context | `default.clock.rate` | the graph frequency | The graph frequency, fixed for the engine's life ([the clock](#the-clock)) | `clock.rate.denom` every block sees | "Blocks process at the graph frequency and block size" |
| context | `default.clock.allowed-rates` | [ the graph frequency ] | Only the graph frequency, so nothing switches the graph's rate ([the clock](#the-clock)) | the graph never switches rate | "Blocks process at the graph frequency and block size" |
| context | `default.clock.quantum` | the block size | The block size, fixed for the engine's life ([the clock](#the-clock)) | `clock.duration` every block sees | "Blocks process at the graph frequency and block size" |
| context | `default.clock.min-quantum` | the block size | The block size: the graph never picks another quantum ([the clock](#the-clock)) | the least quantum the graph picks | "Blocks process at the graph frequency and block size" |
| context | `default.clock.max-quantum` | the block size | The block size: the graph never picks another quantum ([the clock](#the-clock)) | the most quantum the graph picks | "Blocks process at the graph frequency and block size" |
| context | `clock.power-of-two-quantum` | `false` | Unset, the engine rounds the quantum down to a power of two, so a block size of 480 would run as 256 ([the clock](#the-clock)) | the quantum is not rounded down to a power of two | "A block size that is not a power of two runs as itself" |
| context | `default.clock.quantum-floor` | the block size | PipeWire clamps a forced quantum to the floor and the limit (defaults 4 and 8192), not to the min and max ([the quantum guard](#the-quantum-guard)) | the least a forced quantum is clamped to | "The quantum floor and limit are the block size: a node forcing a larger or a smaller quantum leaves every cycle at the block size" |
| context | `default.clock.quantum-limit` | the block size | As the floor; and `module-adapter` hands it to every adapter, so a card takes its ALSA buffer from a period count instead ([the quantum guard](#the-quantum-guard)) | the most a forced quantum is clamped to; module-adapter's `clock.quantum-limit`, which sizes an ALSA buffer without a period count | "The quantum floor and limit are the block size: a node forcing a larger or a smaller quantum leaves every cycle at the block size" |
| internal clock | `factory.name` | `support.node.driver` | The dummy driver: ear's internal clock, the graph's one driver whatever links or fails ([the internal clock](#the-internal-clock)) | a driver on a timer, with no device | "Every block is in the ear.graph group and processes on the dummy driver alone" |
| internal clock | `node.name` | `Dummy-Driver` | How the tests and the logs find it ([the internal clock](#the-internal-clock)) | nothing | "Every block is in the ear.graph group and processes on the dummy driver alone" |
| internal clock | `priority.driver` | `200000` | The highest of every driver's, so the dummy collects the group and everything linked to it first ([the internal clock](#the-internal-clock)) | PipeWire walks drivers in its order: above a card's, so a linked card follows | "A playback card becomes ready, every channel's link active, and then follows, the internal clock driving it and every port block" |
| internal clock | `node.group` | `ear.graph` | The port blocks' group (`graphGroup`, `engine_names.hpp`) ([the internal clock](#the-internal-clock)) | the blocks' group, so this driver drives them all | "Every block is in the ear.graph group and processes on the dummy driver alone" |
| internal clock | `clock.id` | `monotonic-raw` | `CLOCK_MONOTONIC` is slewed by NTP and a timerfd cannot arm `CLOCK_MONOTONIC_RAW`, so the driver tracks raw time with a delay-locked loop ([the internal clock](#the-internal-clock)) | tracking mode on `CLOCK_MONOTONIC_RAW`; `clock.name` `clock.system.monotonic-raw` | "Each cycle carries its start on both clocks, the internal clock as its driver, its rate and the next cycle's start, and only the first is a discontinuity" |
| internal clock | `resync.ms` | `1000000000` | About 11 days, beyond any stall: a reset steps the position back and slews the graph for tens of seconds ([the internal clock](#the-internal-clock)) | the driver never resets after a stall | "After data-thread stalls past 10 ms the internal clock catches up in whole blocks, never steps back, and keeps its phase to the raw clock" |
| block | `node.name` | the node's name | `pw_filter_new`'s name becomes `media.name`; `node.name` would otherwise be the program's ([port blocks](#port-blocks)) | how the registry, the live graph and link-factory name the block | "Live ports name their node, port and direction" |
| block | `media.name` | the node's name | `pw_filter_new` would set it from its name anyway; set here so every property the block carries is a row ([port blocks](#port-blocks)) | nothing | "Every property ear sets is held by PipeWire as the property table says" |
| block | `media.type` | `Audio` | A filter's media type, as any audio filter's ([port blocks](#port-blocks)) | nothing | "Every property ear sets is held by PipeWire as the property table says" |
| block | `node.group` | `ear.graph` | One group, so one driver clocks every block ([the internal clock](#the-internal-clock)) | the internal clock's group, so it drives every block | "Every block is in the ear.graph group and processes on the dummy driver alone" |
| block | `node.always-process` | `true` | The graph always runs: an unlinked always-processing node is assigned to the first active driver ([port blocks](#port-blocks)) | an unlinked block still runs, on the first active driver | "The engine runs with no daemon, runtime directory or D-Bus, and an unlinked sink block processes zeros" |
| block port | `format.dsp` | `32 bit float mono audio` | Every port is one 32-bit float mono channel ([port blocks](#port-blocks)) | a DSP port of one float channel | "A link carries a source block's ramp to a sink block sample for sample" |
| block port | `port.name` | the port's name | `<bank>.<member>`, the name routes and companion links use ([port blocks](#port-blocks)) | how the live graph and companion links name the port | "Live ports name their node, port and direction" |
| card | `factory.name` | `api.alsa.pcm.sink or api.alsa.pcm.source` | Playback on an inside output bank, capture on an inside input bank ([sound cards](#sound-cards)) | the ALSA PCM node module-adapter wraps in a converter | "A playback card becomes ready, every channel's link active, and then follows, the internal clock driving it and every port block" |
| card | `api.alsa.path` | `hw:CARD=<card id>,DEV=0` | Built in one place from a `CardId`, whose grammar keeps every other ALSA device form out, because ALSA's `file` plugin can run commands ([sound cards](#sound-cards)) | device 0 of the card | "A card that cannot open is lost after make returns, its node already gone, and the blocks keep running" |
| card | `audio.channels` | the bank's channel count | The bank's ports, channel for channel ([sound cards](#sound-cards)) | one adapter port per channel | "What a port block sends to inside output to_card.2 lands on the card's channel 2" |
| card | `audio.rate` | the graph frequency | The card runs at the graph frequency; as a follower it is resampled all the time ([sound cards](#sound-cards)) | the rate the PCM opens at | "A playback card becomes ready, every channel's link active, and then follows, the internal clock driving it and every port block" |
| card | `audio.position` | `[ AUX0 AUX1 ... ]` | AUX positions, so the ports the card configures itself are numbered ([sound cards](#sound-cards)) | the ports `playback_AUX<n>` and `capture_AUX<n>` | "What a port block sends to inside output to_card.2 lands on the card's channel 2" |
| card | `node.name` | `<node>.playback or <node>.capture` | The engine companion's name ([engine companions](#engine-companions)) | how the lifecycle names its links' node | "A card joins its block by the block's id: beside an older block of the same name it reaches only the newer" |
| card | `node.always-process` | `true` | Alone, before it is linked, it runs as its own driver ([engine companions](#engine-companions)) | it runs alone before it is linked, so RUNNING means its PCM runs | "A card running alone holds no port block: they stay on the dummy until it is ready" |
| card | `priority.driver` | `1` | `cardPriority` (`engine_names.hpp`) must stay below the internal clock's ([the internal clock](#the-internal-clock)) | below the internal clock's 200000, so linked it follows | "A playback card becomes ready, every channel's link active, and then follows, the internal clock driving it and every port block" |
| card | `api.alsa.period-size` | the block size | With no period count `alsa-pcm.c` caps the buffer at 4 x `clock.quantum-limit`, 4 blocks ([the quantum guard](#the-quantum-guard)) | with `api.alsa.period-num`, the ALSA buffer | "A card's engine node carries no property that forces or locks the graph's quantum or rate, keeps the adapters' old quantum limit for its ALSA buffer, and the extra properties refuse every clock property" |
| card | `api.alsa.period-num` | `cardPeriods(the block size)` | So the buffer stays what it was under the old limit of 8192 ([the quantum guard](#the-quantum-guard)) | the ALSA buffer, not 4 x `clock.quantum-limit` | "A card's engine node carries no property that forces or locks the graph's quantum or rate, keeps the adapters' old quantum limit for its ALSA buffer, and the extra properties refuse every clock property" |
| card seam | `clock.name` | `clock.system.monotonic-raw` | `clocklessCardProperties()`, for CI's test card alone, which has no oscillator: matching would never settle. A real card keeps matching on ([sound cards](#sound-cards)) | a follower whose `clock.name` is its driver's turns rate matching off | "What a port block sends to inside output to_card.2 lands on the card's channel 2" |
| RTP receiver | `remote.name` | `internal` | After `pw_context_connect_self` nothing registers the core object, so the module calls `pw_context_connect` with its `remote.name`; unset, that is the daemon's socket, which ear does not serve. `internal` is protocol-native's connection to the local context (`module-rtp-source.c:671-679`, `module-protocol-native.c:1336-1350`) ([RTP receivers](#rtp-receivers)) | the module's connection is to ear's own context | "A receiver is ready once, every channel's link active, and it and the block follow the Dummy-Driver" |
| RTP receiver | `source.ip` | the rtp node's address | `0.0.0.0` binds any address. A multicast group is joined on the default interface. Any other address binds any address and `connect()`s to `source.ip:source.port`, so it hears only that sender, and only from a source port equal to the receiving port (`module-rtp-source.c:277-283, 314-319`) ([RTP receivers](#rtp-receivers)) | the address the socket binds or connects to | "L24 at the graph rate: channel 1 reaches from_rtp.1 and channel 2 from_rtp.2" |
| RTP receiver | `source.port` | the rtp node's port | The `rtp` node's port ([RTP receivers](#rtp-receivers)) | the UDP port the socket binds | "L24 at the graph rate: channel 1 reaches from_rtp.1 and channel 2 from_rtp.2" |
| RTP receiver | `sess.media` | `audio` | Audio ([RTP receivers](#rtp-receivers)) | nothing | "L24 at the graph rate: channel 1 reaches from_rtp.1 and channel 2 from_rtp.2" |
| RTP receiver | `sess.latency.msec` | the rtp node's latencyMs | The receive buffer, declared upstream of the bank as `beyond` it, since rtp-source reports none ([latency](#latency-per-bank-pair)) | the ring's target fill, rounded down to whole packets | "A real receiver's buffer beyond from_rtp reaches stream whole: the told capture latency's least and most are both the buffer once the receiver is linked and reports its own" |
| RTP receiver | `sess.ignore-ssrc` | `true` | By default the receiver locks onto the first packet's SSRC and drops every other for good, so a sender that restarts (FPP) is never heard again (`module-rtp/audio.c:131-136, 199-206`) ([RTP receivers](#rtp-receivers)) | no lock on the first sender's SSRC | "A sender that restarts with a new SSRC is heard again" |
| RTP receiver | `audio.format` | `S16BE or S24BE` | L16 or L24, big-endian on the wire ([RTP receivers](#rtp-receivers)) | the payload's sample format | "L16 sent at 44100 into the 48000 graph is resampled" |
| RTP receiver | `audio.rate` | the rtp node's rate | The sender's rate ([RTP receivers](#rtp-receivers)) | the receiver resamples it to the graph's | "L16 sent at 44100 into the 48000 graph is resampled" |
| RTP receiver | `audio.channels` | the bank's channel count | The bank's ports, channel for channel ([RTP receivers](#rtp-receivers)) | one port per channel | "L24 at the graph rate: channel 1 reaches from_rtp.1 and channel 2 from_rtp.2" |
| RTP receiver | `audio.position` | `[ AUX0 AUX1 ... ]` | AUX positions name the ports ([RTP receivers](#rtp-receivers)) | the ports `receive_AUX<n>` | "L24 at the graph rate: channel 1 reaches from_rtp.1 and channel 2 from_rtp.2" |
| RTP receiver | `node.name` | `<node>.rtp` | The module makes the node through its own connection, so ear finds it by name ([RTP receivers](#rtp-receivers)) | how ear finds the module's node in the registry | "A receiver is ready once, every channel's link active, and it and the block follow the Dummy-Driver" |
| RTP receiver | `node.description` | `<node>.rtp` | Replaces the host name the module would use ([RTP receivers](#rtp-receivers)) | link-factory matches descriptions too | "A receiver joins its block by the block's id: beside an older block of the same name it reaches only the newer" |
| RTP receiver | `media.name` | `<node>.rtp` | Replaces the host name the module would use ([RTP receivers](#rtp-receivers)) | nothing | "A receiver joins its block by the block's id: beside an older block of the same name it reaches only the newer" |
| RTP receiver | `stream.props` | the RTP stream rows | The receiver's own node properties, the RTP stream rows ([RTP receivers](#rtp-receivers)) | the receiver node's own properties | "A receiver is ready once, every channel's link active, and it and the block follow the Dummy-Driver" |
| RTP stream | `node.always-process` | `true` | The receiver runs on the graph's driver before it is linked, so its RUNNING proves its socket is bound (the module opens it when the stream starts); the lifecycle links on that RUNNING ([RTP receivers](#rtp-receivers)) | it runs before it is linked, so RUNNING means its socket is bound | "A receiver is ready once, every channel's link active, and it and the block follow the Dummy-Driver" |
| RTP stream | `adapter.auto-port-config` | `{ mode = dsp monitor = false control = false position = preserve }` | Without it the stream's converter has no format and so no ports at all; with it the ports are `receive_AUX0`.. (`audioadapter.c:2164-2171`, `audioconvert.c:1606-1617`). It works here, unlike on a card's adapter, because a stream's format is fixed. The module's side consumes it: the engine's node never holds it ([RTP receivers](#rtp-receivers)) | the stream's converter makes its ports | "L24 at the graph rate: channel 1 reaches from_rtp.1 and channel 2 from_rtp.2" |
| route link | `link.output.port` | the output port's global id | A route is made by port global id, from the live graph ([no session manager](#no-session-manager)) | link-factory finds the port by id alone | "A link carries a source block's ramp to a sink block sample for sample" |
| route link | `link.input.port` | the input port's global id | A route is made by port global id, from the live graph ([no session manager](#no-session-manager)) | link-factory finds the port by id alone | "A link carries a source block's ramp to a sink block sample for sample" |
| companion link | `link.output.node` | the block's or the companion's global id | By id: a node of the same name still going would otherwise match first, and `link-factory` matches a name against nick, description and object path too ([engine companions](#engine-companions)) | never an older node of the same name | "A card joins its block by the block's id: beside an older block of the same name it reaches only the newer" |
| companion link | `link.output.port` | the output port's name | By name within that node; PipeWire then holds the port's id ([engine companions](#engine-companions)) | a port of that node, by name | "What a port block sends to inside output to_card.2 lands on the card's channel 2" |
| companion link | `link.input.node` | the companion's or the block's global id | By id, as the output node ([engine companions](#engine-companions)) | never an older node of the same name | "A receiver joins its block by the block's id: beside an older block of the same name it reaches only the newer" |
| companion link | `link.input.port` | the input port's name | By name within that node; PipeWire then holds the port's id ([engine companions](#engine-companions)) | a port of that node, by name | "L24 at the graph rate: channel 1 reaches from_rtp.1 and channel 2 from_rtp.2" |

### Properties ear never sets

`forbidden` in `engine_properties.hpp`: no row may set one (a `static_assert`), `engine.conf` may
carry none, and `AlsaSoundCards`' test seam refuses each one on a card with
`std::invalid_argument`, as it refuses a key every card already sets (`priority.driver` among
them). A key ending in `.*` is every key under it.

| Where | Key | Why |
|---|---|---|
| engine.conf | `default.clock.*`, `clock.power-of-two-quantum` | `context.properties` from the file overwrite the properties `Engine` passes to `pw_context_new`, which carry the clock (`context.c:245`, `conf.c:986-990`) |
| engine.conf | `cpu.zero.denormals` | On 1.4.2 it lands on the main thread in this engine and leaks to every thread made later ([denormals](#denormals)) |
| internal clock, block, card, card seam, RTP receiver, RTP stream | `node.force-quantum`, `node.force-rate`, `node.lock-quantum`, `node.lock-rate` | A node that carries one asks the driver for its own quantum or rate, and every block would run at it ([the quantum guard](#the-quantum-guard)) |
| card | `node.group` | A card in the group would follow from creation and never start before it is linked, which a lifecycle that links once RUNNING cannot do ([sound cards](#sound-cards)) |
| card | `node.driver` | `node.driver = false` would not hold: the ALSA node publishes `node.driver = true` again ([sound cards](#sound-cards)) |
| card | `node.link-group` | It would join the card's driving to other nodes' and take it from the internal clock ([the internal clock](#the-internal-clock)) |

## Modules and libraries

`engine.conf`'s `context.spa-libs`: `support.*` (the loop, logging, the dummy driver), `api.alsa.*`
(a card's ALSA PCM node), `audio.convert.*` (the converter the adapter wraps around it). Debian's
`libspa-0.2-modules` holds all three.

`context.modules`, each for one reason:

| Module | Why |
|---|---|
| `libpipewire-module-rt` (`ifexists nofail`) | Realtime priority for the data thread where allowed; a container without the privilege still starts |
| `libpipewire-module-protocol-native` | The protocol ear's own connection speaks, even with no socket |
| `libpipewire-module-client-node` | A `pw_filter` exports its node through it (`filter.c:1682-1713`); without it no port block appears |
| `libpipewire-module-spa-node-factory` | Makes the internal clock |
| `libpipewire-module-link-factory` | Makes route links (by port global id) and engine companions' links |
| `libpipewire-module-adapter` | Makes a sound card: an ALSA PCM node wrapped in a converter |

`libpipewire-module-rtp-source` is not listed: `RtpReceivers` loads one per `rtp` node with
`pw_context_load_module` ([module-run converters](#module-run-converters)). Debian's `libpipewire-0.3-modules` holds it. The internal clock has no
ports, so it never shows as a live port.

## The clock

`Engine` sets the clock as context properties (the `context` rows of
[the table](#properties-ear-sets)): the graph frequency as the one rate, the block size as every
quantum, never rounded to a power of two, and the floor and the limit at the block size too
([the quantum guard](#the-quantum-guard)). A block sees `clock.duration == block size` and
`clock.rate.denom == graph frequency`. Both are fixed for the engine's life: `main.cpp` reads
`ear.conf` once before `Engine::create` and takes both values when both lines read, else the flags
([graph-file.md](graph-file.md#settings-applied-only-at-restart)).

### The quantum guard

The graph has one quantum and one rate, the driver's, and every block is made for at most the block
size (`maxBlock`): a cycle longer than that the block cannot run, so it writes zeros. A node that
carries `node.force-quantum` (or `node.force-rate`, `node.lock-quantum`, `node.lock-rate`) asks the
driver for its own, and PipeWire clamps a forced quantum to `[quantum-floor, quantum-limit]`
(`context.c`), not to `min-quantum`/`max-quantum`. So two guards keep every cycle at the block size:

- **`quantum-floor` and `quantum-limit` are the block size**, so even a forced quantum is clamped
  back to it (`engine_test.cpp` forces four blocks, and a quarter block, on a node in the group: with
  the old limit of 8192 every block stopped running at four blocks; with both at the block size every
  cycle stays one block).
- **No engine companion carries a clock property.** ear builds every property it gives a card or a
  receiver from closed grammars and sets none of these ([never](#properties-ear-never-sets));
  `AlsaSoundCards`' test seam (its `extra` properties) refuses each with `std::invalid_argument`
  rather than stripping it after the fact, and refuses as well what would take driving from the
  internal clock.

**A card's ALSA buffer does not follow the limit.** `module-adapter` hands every node it makes the
graph's limit as `clock.quantum-limit`, and an ALSA node with no period count caps its buffer at
`min(hardware maximum, 4 x clock.quantum-limit)` (`alsa-pcm.c`, `spa_alsa_set_format`): at a
256-frame block that is 1024 frames (about 21 ms, with `max_delay` half of it), where the old limit
gave 32768. So `AlsaSoundCards` makes every card with `api.alsa.period-size` = the block size and
`api.alsa.period-num` = `cardPeriods(block)` ([the table](#properties-ear-sets)), which takes ALSA's
period-count path instead (`pw_context_update_properties` after creation does not change what
`module-adapter` reads on 1.4.2, so raising the limit later is no way out). ALSA
halves a batch card's period (a USB card such as the MOTU: block / 2, as before), so a batch card's
buffer is again `min(hardware maximum, 4 x max(block, 8192))` frames, its period and headroom (and
so its latency) unchanged; a card that is not batch gets periods of one block and twice that buffer,
capped by the hardware (the test card: 65536 frames, 256 periods of 256, where it had 32768). ALSA's
info log shows it (`PIPEWIRE_DEBUG="2,spa.alsa:4"`: `buffer frames ..., period frames ...,
periods ...`); `sound_card_test.cpp` pins the properties.

Cards (the test card) and RTP receivers run at that limit, a receiver whose sender is at 44.1 kHz
or 96 kHz into a 48 kHz graph included (`sound_card_test.cpp`, `rtp_receiver_test.cpp`). A real
card at that limit is checked only on the hardware (the `ear-on-talos` skill).

### The internal clock

**The graph is always driven by ear's internal clock, the dummy driver, whatever links or fails.**
Every port block sets `node.group = ear.graph`, and so does the dummy. PipeWire puts every node of
one group on the same driver and walks drivers in `priority.driver` order: the dummy (200000) comes
first, so it collects the group and everything linked to it, and a linked card (priority 1,
`cardPriority`) is a follower. So no card ever moves the clock: a card's failure costs only that
card's audio.

**It tracks `CLOCK_MONOTONIC_RAW`.** `CLOCK_MONOTONIC` is slewed by NTP (chrony may slew it by up to
83,333 ppm), which every card's follower loop, every RTP receiver and `sendspin` would have to
chase; and a timerfd cannot be armed on `CLOCK_MONOTONIC_RAW` (`EINVAL`). With `clock.id =
monotonic-raw` the driver runs in its tracking mode (`node-driver.c`): its timer stays on
`CLOCK_MONOTONIC`, a delay-locked loop steers it so `clock.position` follows raw time, and
`rate_diff` is the loop's correction, its error clamped at 1 ms a cycle. `clock.nsec` and
`next_nsec` stay `CLOCK_MONOTONIC`. Its `clock.name` is `clock.system.monotonic-raw`. Whether it
follows a host slewed at chrony's worst case is untested; the fallback is chrony's `maxslewrate`.

**`resync.ms` is set beyond any stall, so the driver never resets.** With the default (10 ms), past
it after a data-thread stall, `on_timeout` puts the position on raw time but leaves its timer where
it was. The timeouts it then fires back to back (the timer catching up) each add a block while raw
time barely moves, so the next reset steps the position **back** by up to about 440 frames, and the
burst leaves it up to 10 ms ahead of raw time; the loop, at its least bandwidth, then slews the whole
graph by hundreds of ppm for tens of seconds (measured: +190 frames and -230 ppm 9 s after one 20 ms
stall). Without the reset the driver catches up on its own timer, a block a timeout, the position
never steps back, and the phase to the raw clock is kept within a frame after stalls of up to a few
seconds. Which of the burst's cycles a block runs is the host's: each timeout re-arms the timer
already due, after it has triggered the graph, so a block runs a cycle only when its wake is
dispatched before that timer's. On the dev host the blocks run about every other timeout, each
cycle they run skipping one block; on a GitHub runner they ran every timeout of a 200 ms and a 1 s
stall, losing nothing. Only the stalled cycle's own cut is certain
([the one data loop](#the-one-data-loop-and-invokes)).

**When it stops.** The dummy stops (state `IDLE`) only when it drives no node at all; a node made
inactive (`pw_filter_set_active(false)`) is skipped and the driver runs on, so that node, active
again, has missed cycles none of which were cut, its position moved by exactly their frames. Started
again, `do_start` clears `last_time`, so the first timeout puts the position on the raw clock instead
of moving it on by a cycle: no cycle missed, the position moved by the raw time it stood still, which
is less than a cycle (a step back) when the gap is short. Every block is always-processing, so the
driver never restarts under a running block (S6).

**The limit.** The catch-up burst after any pause is inherent to `node-driver.c` whatever
`resync.ms` is (about 187 timeouts per second of pause at 48000/256), and each feeds the loop its
clamped 1 ms error, so a pause of tens of seconds or more winds it up and slews the graph by 0.1 to
0.6% for minutes (simulated: 30 s, +287/-429 ppm for about 60 s; 120 s, 1700 ppm at peak). A 3 s
pause stays within 150 ppm from 5 s on. The default `resync.ms` is somewhat better only for pauses
of minutes; a large finite value is worse than both (it resets mid-burst and steps back again and
again). The real fix is upstream: a reset that re-arms `next_time` from now and calls
`spa_dll_init`. A `spa.driver ... err ... > max_resync ..., resetting` warning means `resync.ms` was
lost.

### `GraphCycle`

The block makes one `GraphCycle` a cycle (`port_blocks.cpp`, `onProcess`), so no type reads a clock:

| Member | From |
|---|---|
| `startNs` | `clock.nsec` (`CLOCK_MONOTONIC`) |
| `startRawNs` | The same start on `CLOCK_MONOTONIC_RAW`: both clocks read (mono, raw, mono, the offset taken from the midpoint, as `node-driver.c` does) and the difference added to `clock.nsec`, so a late wake cancels out |
| `position` | The driver's frames at the cycle's start |
| `rateDiff` | `clock.rate_diff` |
| `driverId` | `clock.id`, the driver's global id: another driver is another clock |
| `nextNs` | `clock.next_nsec` |
| `overdueNs` | How long the next cycle had already been due as the block began this one (`cycleOverdueNs`: the monotonic reading taken for `startRawNs` less `clock.next_nsec`, 0 when not yet due). Above 0 only in a catch-up's burst, whose cycles start on the driver's time behind raw time, whether the block runs each of them or misses some ([the internal clock](#the-internal-clock)). A field, not a cause: an overdue cycle lost nothing, and `facts.whole()` says so |
| `facts` | What the cycle states ([writing-a-node-type.md](writing-a-node-type.md#lost-cycles)): `CycleAccount` over the tick (`clock.id`, the cycle's number, `clock.position`, `clock.duration`, `startRawNs`) and the engine's record of the internal clock's cuts (below). The one home of every fact about lost cycles: `facts.discontinuity()` is the block's first cycle, a new code's first cycle, a new driver, cycles missed or any position jump, and the cycle after one too large for the block; `facts.jumpFrames` the position against the last cycle's end. PipeWire gives a filter no xrun count (`clock.xrun` is set only by a driving ALSA node); the facts say each loss, and every consumer (a source's tap, a sink's stamp, a code) reads them rather than working the position out itself |
| `downstream` | The latency downstream of each output port ([below](#latency-per-bank-pair)) |
| `ranRawNs()` | Not a field: when the block really began the cycle, on the raw clock. `startRawNs` when it began on time; when overdue, `startRawNs + (nextNs - startNs) + overdueNs`, the next cycle's due time plus how long that was already overdue, moved onto the raw clock (the two clocks differ by parts per million over the few milliseconds between). A `nextNs` of 0, a test's own cycle, is on time. A node that must date something by when a burst's cycle really ran reads it here; `sendspin` does for the one report its bound forces ([node-types.md](node-types.md#clock)) |

**Why one home.** Before the facts, every consumer worked a loss out again from what it could see
(the position against its own last end, a driver id, its ring's capacity), each by its own rule, and
each timing test had its own judge whose excuse was a window or a shape (a second and a half, 600 ms,
whole blocks within a frame), each looser than what it excused: a bug that dropped a block inside a
window passed. The driver keeps its own account (its realtime `incomplete` event), so the block
states the facts once from it, `GraphCycle` carries them to every consumer (one carrier, no second
view: each consumer already holds the cycle), and one judge excuses exactly the cycles the driver
cut ([testing.md](testing.md#the-cycle-judge)).

**The internal clock's cuts.** `Engine::create` finds the dummy by its `node.name`
(`engine_names.hpp`'s `internalClockNode`, the property table's row; none is an `EngineError`) and
adds `DriverCuts`, a realtime listener on its `start` and `incomplete` events, through the callback
boundary's `listenRealtime` ([architecture.md](architecture.md#the-callback-boundary)), before any
block exists: the add is an invoke the data loop runs before any block's node reaches the driver,
so every block's first cycle is numbered. `node_ready` (`impl-node.c`) tells `start` after
`clock.cycle++` and before any follower is triggered, and a cycle's `incomplete` in the next
`node_ready` before that cycle's `start`, all on the data thread: so inside cycle N every cut of a
cycle before N is recorded. The listener tells a `CutRing` (`ear/core/cut_ring.hpp`, the type
harness's record too), with no allocation, lock or wait: a cycle's start is a relaxed load of the
count and of the last cycle's bit, a release store of the newest whole cycle when that one was not
cut, a release store of the count, a release fence and an atomic AND clearing the new cycle's bit;
a cut is an atomic OR setting the running cycle's bit and, the first time, a release store of the
count of cuts. Cycles are numbered by the
listener's own count from the engine's start (`Engine::cuts().started()`), never by `clock.cycle`:
`incomplete` names no cycle, and the server side's position is private to `impl-node.c` (a
filter's mapping of it goes with the filter, S6). A block names its cycle by that count when
`clock.id` is the internal clock's (`Engine::internalClockId`), else 0. Removing the listener is a
blocking invoke on the data loop, so on a wedged loop the hook is left on the clock and the boundary
keeps it and the ring it tells for good (`RealtimeListener`), as everything that loop holds is
leaked.

The dummy's `clock.delay` is always 0, followers or not: a following ALSA node writes its delay into
its own clock, never into the driver's position.

### Counters

ear keeps running totals of what its cycles state, as core diagnostics
(`ear/core/cycle_counters.hpp`), at two scopes because they mean different things. **The graph's**
say what the host did to the whole graph: a cycle the driver cut is a dropout on every output at
once, which no node caused. **A node's** say what happened to that node beyond the graph's own
losses. Each fact is counted once, where it is stated: repeating the graph's cuts on every node
would make each look faulty for what the host did, and summing nodes' misses into the graph would
count one dropout once per node. The owner asked for counters like these in ear's core diagnostics
(2026-11-10) and asked whether per node or per graph; both, each counting what it means, is the
design's answer, which the owner may still overturn.

| Counter | Scope | Counts | Where it is counted |
|---|---|---|---|
| `cycles` | graph | Cycles the internal clock started | The record of its cuts (`CutRing`'s `started`) |
| `cutCycles` | graph | Of them, those the driver cut short | The record (`cutsSoFar`) |
| `stalls` | graph | Times the graph fell behind and lost cycles: one stall of the host, however many cycles it cut and however its catch-up's burst came out | The record: a cut while no stall is in progress starts one; it lasts until a cycle starts on time, its next not yet due (`overdueNs` 0, told by the blocks: below) |
| `lostMs` | graph | The time the graph lost | Never kept: `GraphCounts::lostMs` encodes it from `cutCycles`, a block each ([the quantum guard](#the-quantum-guard)) |
| `missedCycles` | node | Cycles its block missed that the driver did not cut (the block was not scheduled: paused) | The block, from each cycle's facts: `missed - missedCut` |
| `overruns` | node | Cycles in which the block's own work took longer than the cycle | The block: `cycleOverran`, its work timed from the monotonic reading `onProcess` takes for `startRawNs` to one at its very end (sources, code, fade, taps, metering), so a late wake (`overdueNs`, the host's) is never the node's. It is wall time: the host preempting the data thread during the block's work counts as an overrun of the block's |
| `repositions` | node | Cycles stating `reposition` | The block, from the facts |
| `driverChanges` | node | Cycles stating `driverChange` | The block, from the facts |

`repositions` and `driverChanges` stay 0 on the tracking dummy (`resync.ms` is set beyond any stall
and the driver never restarts under a running block, [above](#the-internal-clock)), so a non-zero
value is a finding. A node's `missedCycles` may count one cycle more than the block missed uncut in
the one case its facts leave undecided (a pause past the record's reach with some cuts in it:
`missedCut` a lower bound, [writing-a-node-type.md](writing-a-node-type.md#lost-cycles)).

**The graph's** run from the engine's start: `Engine::graphCounts()` reads them from the record of
the internal clock's cuts, which counts the stalls beside the cuts. A stall is defined by the fact the
graph is behind, not by cuts being adjacent: after a stall the driver catches up in a burst of
timeouts whose cycles are cut or run whole as the host dispatches them (every one cut, every other
one, or none but the stalled cycle: [above](#the-internal-clock)), so counting runs of consecutive
cuts would make one stall of the host one or many by the host's burst shape. Every cycle of the
burst starts overdue, its next timeout already due, and the first cycle that starts on time ends
the stall. The driver's `start` event carries no time and its position is private to
`impl-node.c`, so the record learns it from the port blocks, which state each cycle's overdue time:
a block whose cycle of the internal clock is not overdue tells the record (`Engine::cycleOnTime`),
on the data thread, before that cycle's cut can be told. A cycle no block ran on time leaves a
stall going, so a burst's cut cycles no block ran stay one stall. A reading takes stalls, then
cuts, then cycles, each an acquire load against the writer's release stores in the other order, so
no reading shows more stalls than cuts or more cuts than cycles.

**A known limit: a stall told ended only by a block.** Only a port block running a cycle of the
internal clock on time ends a stall (`Engine::cycleOnTime`). In a graph where every block is paused
(made inactive) and only engine companions run, no stall is ever seen to end: every cut after the
first counts in that one stall, however far apart, until a block runs on time again. ear makes
every block always-processing, so it would take every node's block made inactive at once; nothing
ear does today makes it.

**A node's** are counted by its port block, every cycle its `onProcess` sees (one too large to run
included), after all its work: a `CycleCounters`, four atomics its one writer, the data thread,
loads and stores relaxed, with no lock or allocation; any thread reads them (`PortBlock::counts`).
They run from the node's add: `NodeHost::counts(name)` gives the newest node of the name not yet
gone, its block's counts now, or what its block counted before it went, and survives its restarts,
stops and changes of settings (the block is made once in a node's life). A port-caused replace is
the same node to the operator (one health stream, its dropped frames), so the newer node adds the
older's counts, read from it until it is gone and carried after, and the total never steps back; a
replace by another type is a removal and an add, from zero. Nothing resets a counter but its
owner's end.

### Reporting the counters

The counters reach the control port (the feature `counters`:
[control-port.md](control-port.md#counters)) and standard output (`ear.graph-lost-cycles` and
`ear.node-missed-cycles`: [output-records.md](output-records.md#diagnostic)) through one pacer on
the main loop, `CounterWatch` (`ear/core/counter_watch.hpp`), and so do the companions' counters, a
sink's dropped frames and a source's shortfall (in the node's
[companion list](#companions-in-a-nodes-list), and as `ear.sink-dropped` and
`ear.source-shortfall`). Nothing runs on the data thread for
them: it writes the counters and is never asked anything.

**When they are read.** At the main loop's heartbeat, the 100 ms timer that already beats the
watchdog ([architecture.md](architecture.md#bounds-the-wedged-data-loop-and-the-watchdog)): each
beat reads the graph's counters (`GraphCounterSource`, the engine) and those of every node whose
span of counts is open (`NodeCounterSource`, NodeHost), all lock-free loads. *Why there and not on
an event:* the counters move on the data thread, where telling the main loop of each cut or miss
would be a system call (an eventfd write) in the middle of a stall's catch-up, the moment the
thread is furthest behind; and a timer of their own would wake the loop again for what the
heartbeat already gives. The cost of a beat is a handful of atomic loads and a lookup per node.

**Spans.** A span is one node's counts from its add, starting from zero, to its end, keyed by the
serial of the node that began it ([above](#counters): a port-caused replace goes on in its older
node's span). NodeHost tells where each begins (`countsBegan`: a node added, or a name replaced by
another type or removed and added again) and ends (`countsEnded`, with its final counts, as its
last node goes, before its `nodeGone` or `supersededGone`), and answers a span's counts while it is
open (`NodeHost::spanCounts`), since only NodeHost knows a node's life. A node superseded by a newer
node of its name keeps its span open while it closes, read and recorded on its own, and ends it
with its final counts as it goes; the wire names only each name's newest span, the node a client
holds. So every record of a node's misses is written, and their sums are the totals, in every case.

**What moves.** A *loss* is `cutCycles` or `stalls` of the graph, or any of a node's four.
`cycles` moves every cycle and `lostMs` follows `cutCycles`, so neither is a change on its own.

**The wire's pace.** At a beat where a loss moved since the last `counters.changed`, and a second
has passed since it, one goes out: the graph's counters whole, and the whole counters of each node
whose counters differ from what the client holds for it (`node.added`'s, then what the last
`counters.changed` named). So it is at most once a second, never when nothing was lost, and at most
1.1 s behind a loss.

**The records' pace.** `ear.graph-lost-cycles` is written at a beat where `cutCycles` moved since
the last such record, the cuts held still over the last beat (or have kept moving for 10 s, a host
that never lets the graph catch up), and a second has passed since the last. A stall's cuts all
come after it, the stalled cycle's own and those of the catch-up's burst as the host runs it (the
driver runs on the data thread the stall holds; the burst is all cut on some hosts and runs whole
on a GitHub runner: [above](#the-internal-clock)), so waiting for them to hold still writes a long
stall once, with every cycle it cut, not a record a cycle nor one a second while its burst runs.
`ear.node-missed-cycles` is the same for a node's `missedCycles` and `overruns`, at most once per
10 s per node (a node that keeps missing must not flood the log), and at its span's end at once,
pace and hold waived, so a node removed within 10 s of its last record still says what it missed.
`repositions` and `driverChanges` are the wire's alone. Each record's numbers are what moved since
the last of its kind, beside the totals; `ms` is the difference of the totals' times, so the
records' `ms` add up to `lostMs`.

**The companions' counters** are read at the same beat, each span's newest node's list through
`NodeCounterSource::spanCompanions`. They count per configuration, not per node: a restart's next
sink of a name counts from zero. *Why here and not on a timer of NodeHost's:* the same reason as
above; and a paced counter's report is the pacer's, as a node's misses are.

- **The list.** At a beat where a sink's `dropped` or a source's `shortfall` differs from what the
  last list `CounterWatch` told for the node named (zero for one it never named), and a second has
  passed since that one, the node's whole list goes to its `NodeCompanionsListener`s, read then;
  only for each name's newest span, the node a client holds. A configuration's end forgets what it
  named, so the next one's companions of the same names start from zero.
- **The records.** `ear.sink-dropped` and `ear.source-shortfall` are paced as
  `ear.node-missed-cycles` is, per companion: once its count held still for a beat, at most once
  per 10 s per companion (`companionDropDiagnosticPace`), and at once, pace and hold waived, as its
  configuration ends. *Why:* an operator learns of drops while they happen, at a bounded rate.
  A count that never holds still (a full disk) is not said at once: its first
  record comes once it has kept moving 10 s, then one about every 10 s while it moves, each with
  what moved since; the companion list shows the count within a second meanwhile (`companionsEnded`, with the final counts),
  so each configuration's records sum to its totals. A superseded node's records are written too.

**At a stop**, every node's span ends as it goes, and once the main loop has ended `flush()`
writes the graph's record still held back (and any companion's still held back), before ear's last record: the records' sums are then
the totals, which the system tests judge from ([testing.md](testing.md#the-counters-own-word)). The pace
is the records', not the counters': the counters are exact whatever the records leave out.

### Denormals

The engine flushes denormals to zero on the data thread (FTZ and DAZ; FZ on arm64) with a blocking
invoke on the data loop (`flushDenormals`). *Why not `cpu.zero.denormals`:* on 1.4.2 it lands on the
main thread in this engine (the data loop's setup runs inline there before the data thread enters)
and leaks to every thread made later. The invoke races the data thread too: `pw_data_loop_start`
returns before the thread enters its loop, and until it does `loop_invoke` runs the function inline
on the caller (about 4% of boots), so `Engine::create` invokes again until it ran on the data thread
(1 ms apart, failing after 5 s). Any blocking invoke made right after `pw_context_new` meets the same
race.

## Port blocks

Each node has one port block, a `pw_filter` named by the node's name (`node.name` set explicitly:
`pw_filter_new`'s name becomes `media.name`), with one 32-bit float mono port per declared port,
public and inside banks alike, every one in the graph group, with `node.always-process = true`.

- **The graph always runs.** An unlinked always-processing node is assigned to the first active
  driver by the context itself (up to PipeWire 1.6; `context.c` 1.0.5:1222,1236-1311). From 1.7.0
  that moved into `module-scheduler-v1`, which `engine.conf` would then have to load.
- **No buffer this cycle.** `pw_filter_get_dsp_buffer` returns null when a port has no buffer
  (`filter.c:2038-2045`): an input then reads zeros, an output writes to scratch.
- **A name added back waits for its older block.** Two blocks of one `node.name` would confuse
  `LiveGraph` and `link-factory`, so a node's block is made only once the older block of its name was
  destroyed, and an engine companion links its block by the block's global id (`pw_filter_get_node_id`,
  read at its first STREAMING), never by name.
- **Destroying a filter is a barrier**: `pw_filter_destroy` removes the node from the data loop with
  a blocking invoke. It is never called once the data loop is wedged (the block is leaked), and
  `NodeHost` batches every destroy and make of a turn into one flush, destroys first, so the main
  loop waits on the data loop at most once a turn.

### Drive and fade

`PortBlock::drive(request, done)` swaps a block's code asynchronously: the request goes into the
block's ring of eight, one non-blocking `pw_loop_invoke` per block in flight applies it on the data
loop between two cycles, and `done(position)` runs later on the main loop, never inside the call,
with the position it takes effect from. Once `done` ran, no `process()` of the old code is running or
will run, so the framework can release the old insides. A request with no code makes every output
silent; the block keeps processing. A block with no data loop applies it on the main thread and
still completes by signal; a block PAUSED or whose driver went still still runs the loop's invokes,
so a drive or fade never waits for a cycle that will not come.

- **Fades.** A drive names which outputs it fades (public outputs, then inside outputs, so a card's
  playback bank fades too); the new code ramps in linearly over 5 ms (`rate * 5 / 1000` frames,
  exact at both ends). `fadeOut(done)` ramps the faded outputs to zero from the next cycle, from the
  gain they have (5 ms from full), the code still running, and completes once a cycle ended at zero,
  or at once on a block not processing; a drive applied while fade-outs wait cancels them (their
  `done` never runs: `NodeHost` drives a silence unfaded when a fade goes unacknowledged). A sink's
  bank is never faded. The ramp and that rule are core's `BlockFade` (`ear/core/block_rules.hpp`),
  beside the stamps a sink's chunk and a source's cycle carry, and each cycle's facts are core's
  `CycleAccount` ([above](#graphcycle)); the type harness's block runs the same.
- **Bounds and the wedge.** A fade not acknowledged in 100 ms is `ear.fade-unacknowledged`; a drive
  or silence not completed in 5 s is `ear.barrier-overdue` and then, once, `ear.data-loop-wedged`.
  Each bound first runs every `done` the data loop already completed, and is judged after a grace
  when the whole process stalled, so a stalled main loop never blames the data loop. Once wedged,
  `Links` makes no link (each refused "the data loop is wedged") and leaks the proxies it would
  destroy, an engine companion leaks its links and body, and a block tells its peers no latency (it
  still publishes it to its own realtime side, only atomics), since each of those reaches the data
  loop through a blocking invoke (`impl-port.c`'s mix release, `port_set_io`) that would hang the
  main loop, SIGTERM included.
- **One invoke in flight per block is a requirement.** An unbounded producer of invokes wedges the
  data loop ([below](#the-one-data-loop-and-invokes)).

### Sinks

A drive carries the sink bindings of its configuration; the invoke copies them into storage sized
when the block was made (nothing allocates on the data loop) and unbinds them in the invoke that
applies the next request. Each cycle the code runs, after the code and the fade, the block writes
each bound inside bank's frames into its sink's frame ring with a stamp (position, both clocks, the
cycle's `causes` and the frames a catch-up lost before it, `upstreamFrames`) in the stamp ring beside it,
and wakes the sink's thread (`pw_loop_signal_event`, an eventfd write; tested to allocate nothing).
So a sink takes exactly the cycles its code drove. The two rings drop together; a full one counts
dropped frames, which `Sink::droppedFrames()` reads at any time (an atomic) and the close reports.
Every rule of the two rings, the drop, the pairing and the cut below, is core's `SinkRing`
(`ear/core/sink_ring.hpp`), which `ThreadSink` and the type harness's sink both wrap. A
sink's first chunk after a block's first drive carries the clock's real position, which may differ
from that drive's take-up position when no cycle ran before it.

`upstreamFrames` is the most capture latency the block last told on the sink's bank
([below](#latency-per-bank-pair)): what reaches the input banks paired with it plus what the
configuration declared on it. Each time the main loop works the block's latency out afresh it
publishes every port's told latency, in frames, as one packed atomic per port, and the tap reads
the bank's at the cycle the chunk was taken. So a consumer dates a chunk without a mailbox: its
first frame entered the graph at `FrameClock(rate).enteredRawNs(startRawNs, 0, {0, upstreamFrames})`
([below](#dating)). A chunk that wraps the ring's end is handed in two calls, and the second's
`position`, `startNs` and `startRawNs` are advanced by the first's frames at the graph's rate (the
adapter is given it; `cutChunk` in `ear/core/sink.hpp`), so the rule holds for each call. The block converts each peer's latency to frames at its port before it combines any
([below](#latency-per-bank-pair)), so when a bank's paths differ the value is the exact most over
them, not PipeWire's field-by-field bound (quanta, frames and ns apart). A wedged data loop stops
the telling to peers, but the block still works its latency out and publishes it, so chunks follow
a new plan.

### Sources

A drive carries the source bindings of its configuration too, bound and unbound in the same invoke
as the code and the sinks, into storage sized when the block was made (one source per inside input
bank). Each cycle the block runs a code:

1. Each port's input span: PipeWire's buffer, or zeros for a port with none.
2. Each bound source plays into scratch of the block's own (one `maxBlock` buffer per inside input
   port, made with the block; the bank's engine ports stay, unlinked), and the code reads that
   scratch for the bank, by the one play rule (`SourceRing::play`) with the source's clock, if it
   has one. Its `SourceTick` (`sourceTick` in `block_rules.hpp`) is the cycle whole (its
   `GraphCycle`, the facts the clock reads), the cycle's frames, the graph rate, and the playback
   latency the block last told on the bank (published per input port as a packed atomic, as each
   output's is for sinks). The ring keeps no time: what it skips is only what the source's clock
   asks ([writing-a-node-type.md](writing-a-node-type.md#a-sources-clock)).
3. The code (or zeros, when none is driven).
4. The fade on the faded outputs.
5. Each bound sink's tap ([above](#sinks)).
6. Metering of the public ports.

A cycle too large to run plays no source and taps no sink; the next cycle states `oversized`, which
a source's clock reads in its tick and decides on (the facts carry no frames for it; the quantum
guard means the engine never makes one: [above](#the-quantum-guard)). The source, the code and its sinks run in one `process`, at one
position, swapped together between two cycles, so a cycle the block does not run none of them runs.
`SourceTap::play` makes no system call but its wakes, each at most one eventfd write and only when
someone waits (a writer for room; the clock wake once until answered, and only when the clock
asks); the clock's calls are realtime-safe by its contract. Tested to allocate nothing with a
writer waiting, a clock told every tick and its wake ringing. Once a stop's silence completed, `NodeHost` tells each
source `stopped()` (its ring dropped); at a teardown's first step it closes each writer, and frees
the sources, each with its clock, after the instance. The rules a source plays by are
[writing-a-node-type.md](writing-a-node-type.md#sources)'s; `EventSources` adds only the writer's
wait (`poll` on an eventfd), the clock wake's eventfd and the clock it owns, and three test seams:
`EventSources::waitsBegun(source)` counts the waits a writer began, each past its last look at the
ring, `waitsWoken(source)` how many of them the room descriptor ended (a play or the close, not the
patience), and `waitDeadline(patience)` is when a wait of that patience ends, so a
test waits for "the writer is waiting" as an event ([testing.md](testing.md#a-writers-own-event)).

## Latency, per bank pair

A port block connects with `PW_FILTER_FLAG_CUSTOM_LATENCY`. *Why:* without it `pw_filter` merges
every output's downstream latency into every input, and every input's upstream into every output
(`filter.c`, `default_latency`), which mixes paths that never meet (a card's playback and capture;
`sendspin`'s `to` and `from`).

What 1.4.2 does under the flag (`filter.c`, `impl-port.c`): the engine recomputes a port's latency
from its peers whenever a link or a peer's latency changes (`pw_impl_port_recalc_latency`) and sets
it on the port; the filter stores it and emits `param_changed` and nothing else (`handle_latency`).
The block then tells its ports' latency itself with `pw_filter_update_params`, which the engine hands
to the peers.

The rules are written once, as a pure model in `ear_core` (`ear/core/latency.hpp`), in frames at
the graph's rate. The block works its latency out by it and nothing else: at `carryLatency` (the
configuration's plan, given before its code drives, cleared at its teardown) and at each peer's
report, it hands the plan and each port's peer to `workOut`, publishes what comes back to the
realtime side and tells each port's latency to its peers. The type harness calls the same
`workOut` with no peer on any port ([writing-a-node-type.md](writing-a-node-type.md#testing-a-type)).
By the plan, which names roles:

- What lies **beyond** a bank is its ports' peers combined (only ports whose peers reported one),
  plus the plan's `beyond` entry for the bank **added in series** (to the least and to the most):
  what a companion adds beyond an inside bank, such as a module-run converter's buffer or a source's
  ring (`[0, capacity]`), which no engine node reports. *Why in series:* the buffer lies behind the
  companion's own ports, and rtp-source's output ports report a latency of zero once linked;
  combined as one more peer (least of the leasts) every path would read a least of zero. A bank with
  neither comes to zero.
- Each output bank's **downstream** latency is what lies beyond it, published to the realtime side
  as `GraphCycle::downstream`.
- An output bank tells upstream (**capture**) latency: over the pairs that end at it, what lies
  beyond the pair's input bank plus the pair's `onPair` delay, combined; plus its own `onBank`. A
  sink on the bank stamps its chunks with the most of it ([sinks](#sinks)).
- An input bank tells downstream (**playback**) latency: over the pairs that start at it, what lies
  beyond the pair's output bank plus the pair's `onPair` delay, combined; plus its own `onBank`. A
  source on the bank tells its clock the most of it, its tick's `playback` ([sources](#sources)).
- A bank in no pair tells its `onBank` alone. A paired bank with no peer is a zero path through its
  pair (an empty combination finishes at zero), so it lowers the least of what the pair's other side
  tells.

`NodeHost` makes the plan at configure's return and carries it with each drive: `pairs` are the
pass-through's; `beyond` holds each source's ring and each module-run converter's buffer
(`moduleRunDelay`, [module-run converters](#module-run-converters)). A typed
`declareLatency(from, to, delay)` makes `from` to `to` a path, with `delay` as its `onPair`, so
the delay counts both ways on it (as PipeWire adds a node's process latency to both), in what `to`
tells and in what `from` tells. For a code of the type's own the pair is added to `pairs`; a
pass-through must already copy it, or the node fails at configure's return (a declared path must
exist). Typed declarations are keyed by their path, so a mixer declares each input's path into one
output. A legacy declaration on one bank (`sendspin`'s) is `onPair` on the
pass-through's pair that ends at the bank or, on a bank no pair ends at, `onBank`. NodeHost works
the plan out with no peer before configure's return is accepted, so a plan that does not fit the
ports fails the node `start-failed` (`configure gave a latency plan that does not fit: <why>`)
instead of reaching a drive.

- **Series and parallel are different types.** `Latency` adds only in series (`+`, saturating).
  Side by side, paths combine only through `Paths`, PipeWire's rule (`spa_latency_info_combine`:
  the least of the leasts, the most of the mosts); an empty `Paths` is **no path**, which is not
  zero, and only `orZero()` (what `spa_latency_info_combine_finish` does) makes it zero. What lies
  in series on every path (a `beyond` entry, a code's delay) is a `Delay`, which `Paths` refuses at
  compile time, so a buffer combined as one more peer does not compile.
- **One unit.** A peer's `{quantum, rate, ns}` converts to frames at the port, once, as the block
  hears it, in `ear_pipewire`'s private `latency_spa.hpp` (`fromSpa`: a quantum is the block size,
  ns are at the graph frequency, rounded to the nearest frame; an unfinished empty combination is no
  path; a negative total is zero), and what a block tells goes back in rate units only (`toSpa`).
  Combined in frames, peers in mixed units give the exact least and most, inside PipeWire's per-unit
  bound; in one unit the two agree, which `ear_pipewire_tests` checks against PipeWire's own
  functions. A negative peer is clamped at the port, before anything in series is added to it: a
  peer reporting -100 frames behind a receiver's 480 tells 480 (summed in PipeWire's units first, as
  the block did before the model, it told 380).
- **A plan that does not fit** the block's ports (a bank it lacks, a pair backwards, a pair delay
  off the plan's pairs, an entry twice) is `workOut`'s `std::logic_error`. NodeHost meets it at
  configure's return (above); the block's `carryLatency` still throws it, on the main loop, so a
  plan from anywhere else is refused before it is kept. After it only the peers change, so no
  peer's report meets a bad plan inside a callback.
- **Dating** is the model's too: what the latency says becomes a time in one place
  ([below](#dating)).

The model's table of bank shapes (`test/support/latency_table.hpp`) is run through `workOut` in
`ear_core_tests` and through a real block in `ear_pipewire_tests`, every port linked to a real
engine node (a peer telling in mixed units where the row gives one, a listener that is no path
elsewhere) and every inside bank read through a sink or a source, so the block cannot drift from
the model ([testing.md](testing.md#what-each-test-program-proves)).

The pairs are the pass-through's, so a card block forwards the card's playback latency from
`to_card` to `play`, and its capture latency from `from_card` to `capture`. An RTP receiver's
`latencyMs` is its `beyond` entry on `from_rtp` (`moduleRunDelay`; rtp-source's ports report zero,
not the buffer, resampling or not),
which the framework adds and `rtp`'s pass-through carries to `stream`. A configuration declares its
own with `declareLatency`: `sendspin` its feed on `from` (in no pair: `onBank`). Frames are at the
graph frequency. ALSA's playback node reports one quantum plus its headroom (`alsa-pcm-sink.c`,
`alsa-pcm.c`): 256 frames on the test card at a 256 quantum.

**Trap: the last link going is never told.** When a port loses its last peer the engine sets a null
Latency param, and `handle_latency` returns on null before it emits anything, so a block keeps the
latency its last peers reported until another peer reports. A lost card leaves its latency on the
card block's inside ports (harmless while nothing hears what goes there). A test checks the trap, so
a PipeWire that fixes it fails it.

### Dating

`FrameClock` (`ear/core/latency.hpp`) is the one code that converts between frames and time at the
graph's rate. It is made only from a `SampleRate` (`ear/core/sample_rate.hpp`; its constructor from a
number is deleted): a rate in hertz with no arithmetic, made only by `SampleRate::of(hz)` (none for
0) or `SampleRate::fixed<hz>()`, comparing only with another rate and converting to nothing. Every
rate ear holds is one: the graph's format (`AudioFormat::rate`), `GraphSpec`'s and the engine's
settings', the fades', the rings', a source's tick, a card's, and an RTP sender's own rate
(`RtpReceiverShape::rate`, which is not the graph's and is never made into a `FrameClock`). The
graph frequency is text only as `ear.conf` or `--rate` writes it (`48khz`), and becomes a rate where
it is read ([graph-file.md](graph-file.md#rates)).

- **`ns(frames)`** is how long frames last, and **`frames(duration)`** how many whole frames a
  duration holds. Each is the exact quotient of the whole count, truncated toward zero (one
  division, never one per part, so two frames at 44.1 kHz are 45,351 ns, not 2 × 22,675), and
  converts whole seconds and the rest apart, so no count overflows on the way: a week of frames at
  96 kHz converts exactly where `frames * 1e9` would leave `int64`.
- **`framesCeil(duration)`** rounds up, exactly for any period (a thirtieth of a second is
  `ceil(rate / 30)` frames), and **`framesNearest(ns)`** rounds to the nearest frame, halves away
  from zero, worked out in integers.
- **`heardRawNs(startRawNs, offset, downstream)`**: frame `offset` of a cycle that started at
  `startRawNs` is heard once its downstream's most has played. `sendspin`'s progress uses it, and so
  does a source's clock that dates what played, from its tick's start and `playback`
  ([writing-a-node-type.md](writing-a-node-type.md#a-sources-clock)); the framework dates nothing
  for a source.
- **`enteredRawNs(startRawNs, offset, upstream)`**: that frame entered the graph its upstream's most
  before. `offset` may be negative, a frame taken before the cycle's first. A sink's chunk entered
  at `enteredRawNs(startRawNs, 0, {0, upstreamFrames})` ([above](#sinks)); `sendspin`'s capture is
  dated from its newest anchor with no upstream yet (its `to` bank is not a sink's, so the capture
  latency a card adds is not counted yet: [node-types.md](node-types.md#clock)).

The rule is a type, not a guard: since a rate has no operator, using one as an operand of `*`, `/`,
`%` or `+` does not compile, nor does making a rate from a number, a `FrameClock`, a format or a
ring at a number, or reading a rate as a number
(`src/core/tests/compile_fail/sample_rate_bad.cpp` holds each case). A peer's latency in PipeWire's
units is converted at the port through `framesNearest` (`latency_spa.hpp`,
[above](#latency-per-bank-pair)) and the level window through `framesCeil` (`levelWindowFrames`,
[below](#levels)). A rate's number leaves only through `hz()`, for a library or the wire, and the
check `ear_sample_rate_exits` (`test/sample_rate_exits.cmake`) refuses the word `hz` anywhere under
`src/` outside tests but in the files it lists: the type, `FrameClock`, and the edges (the engine's
clock properties, a card's and an RTP receiver's `audio.rate`, the WAV writer, `sendspin`'s library
adapter, the wire's encoder). A new edge is a file added to that list. `ear_core_tests` checks
`FrameClock` against the exact quotient worked out in 128 bits, at every rate from 1 Hz to 768 kHz
and at each count where a conversion can go wrong
([testing.md](testing.md#what-each-test-program-proves)).

## Engine companions

An engine companion is a PipeWire node a node's configuration asks for, joined channel for channel
to the ports of one of its block's inside banks and torn down with the configuration. Sound cards
and RTP receivers are the two kinds; an RTP receiver is a module-run converter
([below](#module-run-converters)). A type asks with its bank and a shape (`companions.hpp`:
`SoundCardShape{card}`, `RtpReceiverShape{...}`); a card's direction is its bank's (playback on an
inside output bank, capture on an inside input bank), and an inside bank has at most one companion
of any kind ([writing-a-node-type.md](writing-a-node-type.md#engine-companions)). `NodeHost` works
out each one's `CompanionTarget` (its engine name, the block, the bank's direction, ports and name) and,
once the block is ready, hands it with the shape to the kind's `EngineCompanionAdapter`
(`AlsaSoundCards`, `RtpReceivers` = `ModuleConverters<RtpReceiverShape>`), which tells `ready` and
`lost` through `EngineCompanionEvents`.
Both kinds share one lifecycle (`NodeHost` in `ear_core`, `joinEngineCompanion` in `ear_pipewire`),
and a kind says only how its node is made, how ear hears from it, and what fails it:

1. **Alone.** The engine companion is made and runs on its own until it reports RUNNING.
2. **Linked.** One link per channel through `link-factory` (`link.output.node`, `link.output.port`,
   `link.input.node`, `link.input.port`): the engine companion and the block each by its **global id**
   (the block's is `PortBlock::blockId`), ports by name. *Why by id:* a node of the same name still
   going (a restart's old one, or an older block of a name added back) would otherwise match first, and `link-factory` matches a name against nick, description and
   object path too (`module-link-factory.c`).
3. **Ready** once every link is active; the node is `running` only then.
4. **Lost.** Any failure first destroys the links and the engine companion, waits for a `pw_core_sync`
   round trip, and only then tells `lost(detail)`: the node fails `engine-refused: <detail>`, its
   block and routes stay.

Engine companions are named `<node>.playback`, `<node>.capture` and `<node>.rtp`. A module-run
converter's buffer enters the configuration's latency plan as `beyond` its bank (`moduleRunDelay`,
[latency](#latency-per-bank-pair)); a card reports its own through its links.

### Readiness deadlines

A node never waits for good on something that will never be ready. Each engine companion and each
sink's consumer has a deadline from its request (configure's applied return), on the main loop's
timers (`companions.hpp`: `cardReadinessBound` and friends, `readinessBound(kind)`); a source is
ready when made and has none. The wait counts from the request, so time the block took to be ready
counts too.

| Kind | Deadline | Past it, the node fails |
|---|---|---|
| Sound card (either direction) | 10 s | `engine-refused: the card never started: <card id>` |
| RTP receiver | 5 s | `engine-refused: the RTP receiver never ran (UDP port <port> held?)` |
| A sink's consumer | 5 s | `start-failed: the sink <bank>'s consumer did not begin within 5 s` (its `begin` is the type's code) |

Each deadline is an input of its own when its timer fires, and judges then: if its configuration is
still the current one, still readying (not torn down, not failed) and the companion not ready, the
node fails. Readiness heard first, even in the same main-loop turn, wins, and a companion's
readiness cancels its timer. A timer that fires more than 250 ms late (the main loop stalled) posts
its judgement to the main loop once more, so a ready the stall held back, dispatched behind the
timer, lands first. An engine companion never made, because its block was not ready by the
deadline, fails the node `engine-refused: <node>.<suffix> was never made: the node's block was not
ready within <n> s` rather than blaming the card or the port. The failure tears the configuration
down as any other: the flush destroys an engine companion made or not (destroying one tells nothing,
so its `lost` never comes), and a sink is closed (a `begin` still running finishes first; the close deadline covers one that
never returns). Meanwhile `waitingFor` shows `companion` or `consumer` after 2 s. A failed node is
retried only by `node.start`, `node.restart` or a change until recovery is built; a USB card that
takes longer than 10 s to start fails.

### Companions in a node's list

Every companion a configuration asked for, engine or ring ([sinks](#sinks), [sources](#sources)),
is in its node's list, as a client will see it (`companions.hpp`: `CompanionView`): its name
(`<node>.playback`, `<node>.capture`, `<node>.rtp`; a sink's or source's `<node>.<bank>`), its kind
(`companionKindName`: `card-playback`, `card-capture`, `rtp-receiver`, `sink`, `source`), the
inside bank it joins, whether it is ready (an engine companion once linked, a sink once its
consumer began, a source always), and its counter: a sink's `dropped` (`Sink::droppedFrames`), a
source's `shortfall` (`Source::shortfallFrames`), each counted from its configuration's making.
`NodeHost::companionsOf(name)` reads the list of the newest node of the name, in request order, the
counters read then; it is empty before configure returned applied, after it failed, and from
teardown step 1.

**Who tells it, and when.** `NodeHost` tells its `NodeCompanionsListener`s the whole list whenever
an entry is added or removed or its `ready` changes, in this order against the node's health:

- after the `configuring` it was asked in (the end of that input's batch);
- before the health a companion's readiness allows: a ready engine companion or a begun sink is
  told before the readiness check that prepares and drives the code, so no client sees a node
  running with companions it believes not ready;
- empty at teardown step 1 (a restart's, a removal's, a disable's, a failure's), after the
  `node.state` that began it, so a client never waits for the list to learn of a failure; a stop
  keeps the configuration, so its list.

Only the node a client holds tells: neither a node superseded by a newer node of its name nor the
older node of a port-caused replace, as the wire keys a node by its name
([architecture.md](architecture.md#phases-states-and-what-is-wanted)).

*Why a counter rides in its companion's entry:* a count belongs to the crossing that drops, so it is
told with that companion and never as a status member of the node.

A counter's move alone is `CounterWatch`'s to tell, at the heartbeat
([reporting the counters](#reporting-the-counters)), since the data thread moves a sink's drops and a source's
shortfall and nothing tells the main loop of either. Either may tell a list the other already did (NodeHost
compares lists without their counters, `CounterWatch` only with what it told itself); the control port's gateway,
which hears both, sends a node's list only when it differs from the last it sent
([control-port.md](control-port.md#companions)). As a configuration ends (teardown step 5, or
when it is leaked) `NodeHost` tells its sinks' and sources' final counts
(`NodeCountsListener::companionsEnded`), for the records.

### Sound cards

`AlsaSoundCards` makes one ALSA adapter node per direction:

- **Made** with the `card` rows of [the table](#properties-ear-sets), which say why each, and **no
  `node.group`** ([never](#properties-ear-never-sets)). So it runs alone, on its own clock, and holds
  no port block while it opens.
- **Ports.** `adapter.auto-port-config` fails on 1.4.2 for the card's adapter (one unconfigured port
  whose PCM fails to start, EINVAL), so the card sends the port configuration itself, as a session
  manager would: `pw_node_set_param(SPA_PARAM_PortConfig)` in dsp mode with 32-bit float planar and
  AUX positions, naming the ports `playback_AUX<n>` and `capture_AUX<n>`. Block port `to_card.<i>`
  links to `playback_AUX<i-1>`, `capture_AUX<j-1>` to `from_card.<j>`.
- **Following.** Linked to its block, the card joins the blocks' graph, where the dummy outranks it:
  it becomes a follower (one repeated sample at the switch on the test card). As a follower the ALSA
  node runs a delay-locked loop on its buffer against the graph clock and steers the adapter's
  resampler, so every card is resampled all the time (no output is bit-perfect), its rate takes a
  few seconds to settle, and a card's playback and capture each follow with their own loop (their
  relative latency jitters by tens of samples).
- **Lost.** Refused at open, ERROR, a rate the kernel refuses, unplugged, a link that fails or goes.
  A lost card never held the clock, so the blocks keep the dummy and their position.
- **ALSA calls run on the main loop.** The engine runs in ear's process, so a card's adapter opens,
  prepares, drops and closes its PCM on ear's main loop, synchronously. A USB card that wedges in the
  kernel can block the whole main loop until ear restarts; the main-loop watchdog reports it.
- `node.driver-id` changes reach no client on 1.4.2: the engine sets it but sends no info event. A
  test that reads it binds the node again.

**CI's test card.** `src/pipewire/tests/alsa/alsa.conf`, set as `ALSA_CONFIG_PATH`, defines `hw` so
`hw:CARD=Test` is alsa-lib's `file` PCM over its `null` PCM (what the card is sent lands in a file);
any other id cannot open. It has no oscillator, so as a follower its rate match never settles (the
correction climbs). Engine tests therefore make it with `clocklessCardProperties()`, which sets its
`clock.name` to the dummy's: an ALSA follower whose clock name equals the driver's turns matching off
(`alsa-pcm.c`, `setup_matching`), so it is bit-exact. `main.cpp` never passes it: a real card keeps
matching on. Drift tracking, follower resyncs, a real card's latency and its errors on a running PCM
are checked only on the hardware (the `ear-on-talos` skill).

### Module-run converters

A converter companion is PipeWire's converter (audioconvert in an adapter, with its resampler)
between a node's own side and one of its block's inside banks: the one place a clock and a rate
other than the graph's meet the graph ([architecture.md](architecture.md#the-timing-model)). In
**module-run** mode a PipeWire module owns all of it: it makes the stream, keeps its own buffer and
runs the loop that steers its resampler. ear loads the module, finds its node, links it, carries
its latency and judges its life. RTP receivers are the first and only shipped one.

The code is one shared body for every module-run shape, and a description per module:

- **`ModuleConverters<Shape>`** (`module_converters.hpp`) is the shape's `EngineCompanionAdapter`:
  `make` joins the body to the block through `joinEngineCompanion` with the module's kind, flow and
  port names. `RtpReceivers` is `ModuleConverters<RtpReceiverShape>`, instantiated in
  `rtp_receivers.cpp`.
- **`ModuleConverterBody<Shape>`** (`module_converter_body.hpp`) loads the module and finds its node
  (below), hears its state (`tellNodeState`: RUNNING, ERROR) and fails on its node's removal or the
  module's destruction. Its failure details use the module's words: `cannot load PipeWire's <word>
  module: <errno>`, `the engine removed the <kind>`, `the engine removed the <kind>'s module`,
  `cannot watch the <kind>: ...`. A module that will not load is told later, never inside `make`.
- **`ModuleRun<Shape>`** (`converter_modules.hpp`, specialised beside the adapter's instantiation)
  is what is the module's own: `module` (a `ConverterModule`: its `kind` word, its module `word`, the
  `body` name its fault reports use, its `flow` and its node's port for each channel), `Arguments`
  and `arguments(engineNode, shape, channels)`, built only from parsed values, and `load`. A shipped
  module loads through `engine_property_set`'s `loadModule`, so its keys come from
  [the table](#properties-ear-sets) and the property guard holds.
- **`moduleRunDelay(shape, graph)`** (`companions.hpp`, `ear_core`) is the one latency rule: what
  the module buffers beyond the bank, at the graph rate, least and most alike. `NodeHost`'s plan
  enters it as `beyond` the bank for every shape that has one (`ModuleRunShape`), and for no other.
  RTP's is `latencyMs` through `FrameClock`.
- **The readiness deadline and its failure text** stay the shape's kind's, in `NodeHost`
  ([readiness deadlines](#readiness-deadlines)): a node that never runs tells nothing here.
- **Fault reports** name a body's callbacks by its module's `body`, not by the template's spelling
  (`NamesItself` in `callback_boundary.hpp`): RTP's are `RtpReceiverBody::onCoreDone` and its
  siblings, as before the body was shared, so a report says which module's converter threw.

**How ear finds its node.** The module makes the node through its own connection, so there is no
proxy to watch. ear binds a registry on its own connection and sends a `pw_core_sync`; node globals
of the name seen before its done are stale (a restart's old node still going), and the first node
of the name after it that is not stale is bound. A stale id is forgotten when its global goes,
because the engine may reuse the id.

**What the base relies on, measured** (`spikes_test.cpp`, S7, on the real receiver):

- **The module's node finishes before the block wakes, in every cycle.** PipeWire's own profiler
  (`module-profiler`, loaded into a copy of `engine.conf`) over 2,000 cycles of a 44.1 kHz sender
  resampled into the graph: in every cycle both ran to the end, the receiver finished before the
  block was signalled, and neither reported an earlier cycle's run (the stream is not `node.async`).
  So the block reads this cycle's frames from the module, never the last cycle's.
- **The module's ports tell no latency of their own**, at the graph's rate and resampling 44.1 kHz
  alike: rtp-source's `receive_AUX0` tells 0 frames once linked, and the block tells exactly
  `moduleRunDelay` downstream. So the module's buffer enters the plan once, and the resampler's own
  delay is not counted anywhere.

**The card is not a module-run converter.** It is PipeWire's converter too (audioconvert inside the
ALSA adapter, steered by the follower's loop), but ear makes it itself with
`pw_core_create_object("adapter")` on its own connection and holds its proxy, so there is nothing to
find by name; it sends its `PortConfig` itself (`adapter.auto-port-config` fails for the ALSA adapter
on 1.4.2), where a module's stream configures its own ports; it reports its latency through its
links, so it has nothing `beyond` its bank; it flows either way (playback and capture), and its
quantum-guard sizing and test seam are its own ([sound cards](#sound-cards)). What the two share is
the lifecycle, and both already use `joinEngineCompanion`; the module-run body would add a registry
search and a latency rule the card does not use.

**Tested apart from RTP** (`module_converter_test.cpp`) through a test-only shape whose module is
`libpipewire-module-pipe-tunnel` in source mode: like rtp-source it owns its stream, its buffer and
the loop steering its resampler, reads from outside the graph (a FIFO the test writes) and makes its
node on its own connection, but it needs no network or sender thread, its ports are `capture_AUX<n>`
and a bad `pipe.filename` makes it refuse to load. Ready and channel order, the stale-name rule, a
module that will not load, a node that never runs, a node removed, a module destroyed and a fault
named by the module's body are each proved through the shared code in its words.

### RTP receivers

`RtpReceivers` loads `libpipewire-module-rtp-source` into the engine, one per `rtp` node: a
[module-run converter](#module-run-converters) whose `ModuleRun<RtpReceiverShape>`
(`rtp_receivers.cpp`) holds what is RTP's own. Its arguments are built in one place from parsed
values only (an `RtpAddress`, an enum, ranged numbers, the engine node name): the `RTP receiver` and
`RTP stream` rows of [the table](#properties-ear-sets), which say why each. `cleanup.sec` (60),
`net.mtu` (1280), packet times and `local.ifname` are left at their defaults. Its node's ports are
`receive_AUX<n>` (a `node.network` stream's), and its words are `RTP receiver` and `rtp-source`.
PipeWire keeps the jitter buffer, the tracking of the sender's clock and the resampler.

**What PipeWire 1.4.2 does that ear cannot see or change:**

- **A port that cannot be bound is only logged** (`bind() failed: Address already in use`): the node
  is not put in ERROR, not removed, the module not destroyed (`module-rtp-source.c:369-386`,
  `module-rtp/stream.c:228-251`). With always-process the node never reaches RUNNING, so the `rtp`
  node stays `configuring`, waiting for its companion, until the receiver's 5 s deadline fails it
  `engine-refused: the RTP receiver never ran (UDP port <port> held?)`
  ([readiness deadlines](#readiness-deadlines)). The module sets `SO_REUSEADDR`, so two receivers on
  one port both bind.
- **No packets is not a failure.** The module never destroys itself; after `cleanup.sec` without
  packets it sets the node property `rtp.receiving = false` (a bound node proxy sees it, the registry
  does not; `module-rtp-source.c:456-481`). ear does not read it.
- **Datagram size.** The receiver reads into a buffer of `net.mtu` bytes (1280,
  `module-rtp-source.c:715-716`); a longer datagram is cut short and garbled.
- **Any sequence mismatch resyncs**: a packet lost or out of order (`unexpected seq`) zeroes the
  ring and refills it to the target latency, an audible gap of about `latencyMs`
  (`module-rtp/audio.c:131-166`). So does a ring overrun.
- **Rate.** The receiver steers its resampler with a delay-locked loop from packet arrival, so its
  rate wobbles by parts in ten thousand: a sine fit over a whole second fails while every tenth of a
  second fits. Check a received tone window by window.
- **Latency rounding.** `sess.latency.msec` is rounded down to whole packet times (100 became 96.9 ms
  for 2-channel L16). The ring holds 4 MiB.
- **A host name blocks the main loop** in `pw_net_parse_address` (15 s for `fpp.local`) before the
  load fails: hence the closed address grammar.

CI's tests use a sender thread on 127.0.0.1 (`test/support/rtp_tone.hpp`), never PipeWire's
`module-rtp-sink`, and never multicast.

## The one data loop and invokes

Verified by `spikes_test.cpp` (S1 to S3, and S5's part B) against 1.4.2's `loop.c`, `context.c` and `filter.c`:

- **Asynchronous `pw_loop_invoke` from the main loop** runs on the data thread in posting order,
  never inside any `process`, and the post returns the async code, never the function's result. A
  full invoke queue (32 KiB, about 455 items) is never refused: the *posting* thread `calloc`s an
  overflow queue and goes on, and posting order holds across queues; the data thread allocates
  nothing. A loop whose thread is not running runs the function inline on the caller, which is why
  completions are posted, never run inline. **A producer posting as fast as it can outruns the
  flush**: the data loop spins in `flush_all_queues`, never runs `process` again, and memory grows.
- **Take-up by position.** Every request is taken up once, in order, one cycle after publication,
  and nodes enter in link order in every cycle they all run. But a cycle an xrun cuts short is run by
  an upstream prefix of the graph only, never by a downstream node without its upstream; so cycles
  are told apart by the order they ran in, never by position alone.
- **What a node reads after a cut-short cycle** (S5). In the first cycle a node runs after its
  upstream ran one it did not, it reads the upstream's frames of the cycle it runs, never those of
  the cut one: no stale buffer, no lasting shift. The cut cycle's frames are lost downstream, and
  a route between blocks keeps in step across an xrun. Measured with the upstream stamping every
  frame with its cycle's position: 3,491 such cycles the host cut short over ten idle and ten
  loaded runs, and 2,833 over ten runs that cut them on purpose, every one current.
- **A cycle can be cut short on purpose.** A node that sleeps past the cycle inside its `process`
  cuts that cycle short straight after itself, on any host: the driver's timer became ready
  during the sleep, before the eventfd the node writes as it finishes, the loop dispatches them
  in that order, and the next cycle's `node_ready` (`impl-node.c`) resets the woken node, whose
  wake then finds it no longer triggered. S5 makes both shapes this way (U overruns, and B), since
  a host cuts cycles short only by chance (a GitHub runner: 9 in 60 s beside a busy thread per
  core). The driver may cut a cycle or two more catching up.
- **A source's clock sees every cut in its tick's facts** (S5's part A, on a test filter, then on
  the real block in `source_test.cpp`). An upstream probe sleeping 12 ms past the 5.3 ms quantum
  every 50th cycle cuts that cycle short after itself, as above: the block does not run it, and the
  next cycle states a catch-up whose jump is whole blocks within the source's capacity. A clock
  that skips each catch-up's jump has the ring drop exactly that, the node after the block hearing
  each frame of the stream the clock was told it read; a clock that skips nothing (or no clock)
  has the ring play on in order across the cuts. So a source's clock can keep time by the facts
  alone, and the ring keeps none of its own.
- **One data loop.** `context.num-data-loops` reads 1, the process has one `data-loop.0` thread, and
  a filter's `pw_filter_get_data_loop` is that loop. Adapters and the RTP module name `data-loop.0`
  in their info; `pw_filter`s name no loop.
- After a 2 s data-loop stall the driver catches up as [above](#the-internal-clock); the bounds treat
  it as no wedge while the loop answers invokes.
- **The driver reports every cycle a node misses** (S6). Every follower is a target of the driver,
  which counts them, so a cycle any node has not finished when the next timeout comes is reported
  `incomplete` in that `node_ready`, before the next `start` (`impl-node.c`). Over 30 runs of a chain
  and a sibling stalling at planned cycles beside a busy thread per core (180,044 cycles, 40,397 cut),
  every cycle a node missed between two it ran had been reported; none silently. The `TRIGGERED`
  recovery (the last node finished, the driver's own wake not yet run) reports nothing and loses
  nothing; every cycle is told `complete` once. A timeout is one cycle (a one-shot absolute timer,
  its expiration count ignored), and `clock.cycle` moves with every `start`, never reset. No follower
  can read a mark of the recovery: `SPA_IO_CLOCK_FLAG_XRUN_RECOVER` is cleared before any follower is
  triggered, and `clock.xrun` stays 0 on the dummy.

**Data-loop callbacks never throw.** A filter's `process` and every invoke that runs on the data loop
(a block's requests and its blocking flush, the denormal flush, the scheduling read) are registered
through the callback boundary as `realtime<>` on a `DataLoop`, which takes nothing else, whose member
must be `noexcept`, and nothing is added around them; so are the engine log's methods and alsa-lib's
error handler, which may run there too (`realtimeFunction<>`): nothing on the data loop catches, allocates or formats (the scheduling read takes the
policy and priority there and says them on the caller's thread). A throw there is `std::terminate`
([architecture.md](architecture.md#the-callback-boundary)).

## Levels

Each block meters its public ports (never inside ports) every cycle on the realtime thread: sample
peak and RMS, linear (`LevelMeter`, `src/core/levels.cpp`). A window is ceil(rate / 30) frames
(1600 at 48 kHz). A block publishes in the cycle that crosses a multiple of the window on the graph
clock, so every block's windows end at the same positions whenever it was made. Publishing writes
the block's triple-buffer mailbox (no lock, no allocation) and rings the level bell (an eventfd) at
most once per window; only the realtime side remembers which window it rang for, so a bell answered
without reading never silences later windows. `latestLevels()` returns one window for every block
that has one, the oldest latest window, since blocks publish one after another within a cycle.

## Realtime priority

`libpipewire-module-rt` raises the data thread to SCHED_FIFO only if the process may. The run image
gives `/opt/ear/ear` CAP_SYS_NICE as a **permitted-only** file capability (`setcap cap_sys_nice=p`);
a binary marked effective (`=ep`) would fail to start with EPERM wherever the capability is not
allowed. `Engine::create` makes it effective (raw `capget`/`capset`, no libcap) before `pw_init` and
before any engine thread exists. Not permitted: nothing changes, nothing fails. A capability added
by `docker run --cap-add SYS_NICE` or a pod spec alone never reaches a non-root process (execve
clears it), and `no_new_privs` (Kubernetes' `allowPrivilegeEscalation: false`) ignores file
capabilities. `Engine::dataThreadScheduling()` asks the data thread itself, and ear prints the
answer (`engine` record) and `graph.get` carries it.

## No session manager

Nothing like WirePlumber runs. Every block is always-processing (so an unrouted one runs), and ear
makes every link itself (the reconciler, through `Links`). A `link-factory` link belongs to the
connection that asked for it (no `object.linger`), so destroying its proxy removes it; the factory
destroys a link that goes to ERROR; a proxy removed before ACTIVE is a failure, after ACTIVE it is
gone (`module-link-factory.c` 1.0.5:263-277,365-400). ear never destroys a proxy inside its own event.

## Traps

- **A dot in a port's name is safe.** `pw_impl_port_add` (`impl-port.c`) only copies `port.name` and
  joins it into `port.alias`; it never parses it, and a route's link goes by port ids.
- **Stop signals kill the process unless blocked first.** `pw_context_new` starts the data-loop
  thread, and a SIGTERM goes to any thread that does not block it. `Engine::create` blocks SIGINT and
  SIGTERM before `pw_init`, so every thread inherits the mask, and reads them through a signal
  source (`pw_loop_add_signal`, a signalfd, registered through the callback boundary). Threads
  started later (the control port's) inherit it too.
- **Block size silently rounded.** Without `clock.power-of-two-quantum = false` a block size of 480
  runs as 256.
- **Custom properties never reach the registry.** Registry globals carry only whitelisted keys
  (`impl-node.c:59-78`, `impl-port.c:1016-1033`), so ear recognises its nodes by `node.name`, which is
  safe only while no other client can connect (no socket).
- **Existing objects arrive later.** A new registry binding replays every existing global as events
  after the call that made it returns. `LiveGraph` gives each subscriber its own binding and caches
  nothing; a port can arrive before or after its node.
- **Never tell a listener inside the call that caused it.** Refusals in making a block, a link or an
  engine companion are told later through `pw_loop_signal_event` on the main loop.
- **Main-loop work from another thread.** `MainLoopExecutor::post` queues the function and signals
  a main-loop event; posted from the loop thread itself it still runs later. Work left when it goes
  is destroyed unrun. Each item runs through the callback boundary on its own, so one that throws
  never drops the items behind it.
- **An exception through a C callback** unwinds through PipeWire's C frames, which is
  `std::terminate` with no word of where. Every callback is registered through the callback
  boundary instead (`addEvent`, `addTimer`, `addSignal`, `invoke`, `listen`, `installLog`,
  `setAlsaErrorHandler`), never with `pw_loop_add_*`, `pw_loop_invoke`, an `*_add_listener`,
  `pw_log_set` or an interface table of its own: `no_raw_callbacks.hpp`, forced into every source of
  `ear_pipewire`, poisons those after the boundary, so they do not compile outside it ([architecture.md](architecture.md#the-callback-boundary)).
- **A user node named like another engine node** (only `Dummy-Driver` today) gets engine-companion links
  that fail, since the block side of those links is matched by name.

## Xruns on a loaded host are not bugs

When the host is busy (other containers in Docker's VM, a load average far above the CPU count),
PipeWire's data thread can stall for tens to hundreds of milliseconds without a wake; each stall is a
graph xrun. A ramp test then sees a break, and an RTP test a gap (an underrun past `latencyMs`, or a
resync after reordered packets). To tell it from a bug, rerun with `PIPEWIRE_DEBUG="2,pw.node:4"`: an
xrun shows the dummy driver's missed cycles back to back while the late nodes' own process time is
microseconds. Run the same test on the unchanged code in the same conditions; if that fails too, the
host cannot judge it, and CI's runner is the gate. The S2 spike test is `RUN_SERIAL` for this reason.

## Known limits

- A card's ALSA calls run on the main loop ([sound cards](#sound-cards)).
- A pause of tens of seconds or more on the data thread slews the graph off the raw clock for minutes
  ([the internal clock](#the-internal-clock)).
- A cycle an xrun cuts short is run by an upstream prefix of the graph only.
- Latency is only declared at start, never reported while a configuration runs.
- Not built: an internal clock anchored to one card (that card bit-perfect), a follower watchdog for
  a frozen card, cards opened off the main loop, serving PipeWire's socket.

## Bumping PipeWire

The version is whatever Debian trixie's archive holds when the image is built (the Dockerfile pins
the base image by digest, not the packages).

1. Change the source in `docker/Dockerfile`, rebuild through `scripts/dev.sh`, and read the version
   with `scripts/dev.sh dpkg-query -W libpipewire-0.3-0t64`.
2. Read PipeWire's release notes between the two versions for: scheduling of unlinked always-process
   nodes and node groups (from 1.7.0, load `libpipewire-module-scheduler-v1`, and re-check how a card
   joins the group's clock); renamed clock keys; whether `context.properties` still overwrite
   `pw_context_new`'s; `pw_filter` and `link-factory` behaviour; the adapter's port configuration;
   `node.driver-id` events; the registry's key whitelist; module-rtp-source's arguments, failed binds,
   the SSRC lock, the resync and `rtp.receiving`; custom latency's null param; and `node-driver.c`'s
   tracking mode (whether `resync.ms` is still read, and whether a reset now re-arms its timer and
   calls `spa_dll_init`, in which case the default could come back).
3. Check every module `engine.conf` names, and `libpipewire-module-rtp-source`, still exists:
   `scripts/dev.sh bash -c 'ls /usr/lib/*/pipewire-0.3/'`.
4. Run `scripts/dev.sh make test`: `ear_pipewire_tests` cover these facts (among them the property
   table's live check, which fails for any property the new version drops or overrides,
   [above](#properties-ear-sets)) and `ear_system_tests` the whole path. S6 (`[s6]`) checks the
   cycle judge's premise, that the driver reports every cycle a node misses
   ([above](#the-one-data-loop-and-invokes)): if a version loses one unreported, that is a question
   for the owner (carry a patch to PipeWire; excuse such a cycle only by a mark PipeWire leaves on
   it, of which 1.4.2 leaves none; or report it upstream and let the timing tests fail until it is
   fixed), never a judge loosened. The recommendation, since PipeWire leaves no reliable mark to
   excuse by: carry a patch, and report it upstream either way.
5. Update the version named in `engine.conf`'s header, the `pipewire-engine` skill, `AGENTS.md` and
   this page.
