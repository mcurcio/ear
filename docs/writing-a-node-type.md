# Writing a node type

A node type is what a `node <type> <name> { ... }` line in `ear.conf` names: `white`, `file`,
`card`, `rtp` and `sendspin` are the built-in ones ([node-types.md](node-types.md)). This page is
the type API as a library: how a type is laid out and registered, what it declares, the class the
framework makes once per configuration, the rules the framework holds it to, and how it is tested.
It ends with a complete worked example, a `tone` type.

The framework owns everything around a type: the node's port block, its state, fades, barriers,
teardown, failures and timing. A type declares its settings and ports, acquires resources, and
gives the code that runs on the realtime thread. It never calls PipeWire, never writes to standard
output, never reads a clock, and never sets the node's state.

## Layout and registration

A folder registers its type: nothing else names it, not `main.cpp`, not `src/nodes/CMakeLists.txt`,
not a list. Configure looks for folders again on every build and prints `Node types: ...`.

```
src/nodes/<type>/
  CMakeLists.txt                       ear_node_type(SOURCES <type>.cpp TESTS tests/<type>_test.cpp)
  include/ear/nodes/<type>.hpp         one function: std::unique_ptr<const NodeType> make<Type>NodeType();
  <type>_spec.hpp                      the spec and the class, private to the type and its tests
  <type>.cpp                           make<Type>NodeType() and anything needing a library only it links
  tests/<type>_test.cpp                its tests, through TypeHarness
```

- The folder's name is a lower-case letter, then lower-case letters and digits; it is the type's
  name, its library `ear_node_<type>` and its factory `make<Type>NodeType()` (first letter upper).
  **A type's name is 1 or more of `[a-z0-9]`**, never a `-`, so a [store](#stores)'s file,
  `<type>-<node>.<suffix>`, is one type's and one node's alone (`send-spin` with node `x` and `send`
  with node `spin-x` would share one). The catalogue checks it at startup, with two types of one
  name: either ends ear with exit 1, naming the type.
- `ear_node_type()` (`cmake/NodeType.cmake`) makes the library (with a header set checked on its
  own, linking `ear_core`) and, under `BUILD_TESTS`, the test program `ear_node_<type>_tests`
  linking the type harness. A folder without the call, without `SOURCES` and `TESTS`, without its
  public header, or a stray file beside the folders in `src/nodes/`, stops configure.
- Plain CMake after the call adds any outside library the type needs, `PRIVATE`, so no other
  target sees it (`target_link_libraries(ear_node_file PRIVATE PkgConfig::SNDFILE)`).
- **No type links another, nor a package only `main.cpp` links** (`ear_runtime`, `ear_pipewire`,
  `ear_control`, `ear_config`, `ear_server`): configure refuses it once the whole project is read,
  through a helper library, an alias or a generator expression too. A type reaches the engine and
  the framework's threads only through `ear_core`'s interfaces; its test program may link what it
  needs.
- **No type writes to standard output or error**: a CTest test refuses any file under `src/nodes/`
  that names `std::cout`, `std::cerr`, `stdout`, `stderr`, `printf`, `std::print` or writes to
  descriptor 1 or 2. A type speaks through notes (`say`) and failures (`lost`); see
  [output-records.md](output-records.md).
- The generated list (`out/src/all_node_types/all_node_types.cpp`) calls each factory with no
  argument. (`sendspin`'s alone still takes `const NodeFolders&`, an interim seam.)

## The spec

In the type's private header, inside its own named namespace `ear::nodes::<type>` (never an
unnamed one), every constant `inline constexpr`. The header includes `ear/core/lifecycle.hpp`,
which brings in `type_spec.hpp` and `settings_table.hpp`.

*Why a named namespace:* GCC 14 gives a template instantiated on a reference to an object external
linkage even when the object is in an unnamed namespace, so two files' same-named rows or specs
would silently share one table or one `Io`. `SettingsTable`, `Io`, `Members`, `NodeOf`, `CreateOf`
and `makeNodeType` refuse an unnamed namespace at compile time.

### Settings: one table of typed rows

```cpp
inline constexpr auto cardRow = setting::required("card", setting::text<CardId::parse>("an ALSA card id, ..."));
inline constexpr auto playbackRow = setting::defaulted("playback", setting::number(0, 64, "channels"), 0);
inline constexpr auto rateRow =
	setting::optional("rate", setting::rate(SampleRate::fixed<8'000>(), SampleRate::fixed<192'000>()));
inline constexpr auto portRow = setting::optional("port", setting::number(1, 65535)).onlyWithout(serverRow);
```

| Row | Reads as | When absent |
|---|---|---|
| `setting::required(key, rule)` | the value | `<key>: missing` |
| `setting::optional(key, rule)` | `std::optional` of the value | empty |
| `setting::defaulted(key, number, default)` | the value | the default (numbers only; a default outside the range does not compile) |

| Rule | A good value | Refused with |
|---|---|---|
| `setting::number(low, high, unit = {})` | a JSON integer in [low, high] (not a float, bool or string) | `<key>: must be a whole number[ of <unit>], <low>-<high>` |
| `setting::rate(low, high)` | a JSON integer of hertz in [low, high], read as a `SampleRate`; in `ear.conf` written with its unit (`44.1khz`, [graph-file.md](graph-file.md#rates)) | `<key>: must be a rate from <low> to <high> (on the control port, a whole number of Hz)`, the ends as ear writes them (`8khz`) |
| `setting::text<Parse>(mustBe)` | a JSON string `Parse` accepts; `Parse` is `std::optional<T>(std::string_view)`, and `T` (what the row reads) must own what it holds | `<key>: must be <mustBe>` |

- `.onlyWithout(above)` refuses the key given together with a row above it: `<key>: only without
  "<above>"`. Neither row may be required.
- Each key once; `disabled` (the framework's key) cannot be declared; a table's name must be its
  folder's. Each is a compile error.
- The table is `SettingsTable<row1, row2, ...>{"<type>"}` (`SettingsTable<>{"white"}` for none).
  It gives the type's name, its settings' kinds (text, whole number or rate, which is how `ear.conf` is
  read and written) and the whole check, so the graph file reader cannot disagree with `parse`.
- A parsed value is parsed once: a text row reads as what its parse function made (a `CardId`, an
  `RtpAddress`, a path).

### Banks: the ports

```cpp
inline constexpr auto play = bank::input<"play">;             // routable: routes name play.1, play.2, ...
inline constexpr auto capture = bank::output<"capture">;
inline constexpr auto toCard = bank::insideOutput<"to_card">;   // inside: only the node's own companions and code
inline constexpr auto fromCard = bank::insideInput<"from_card">;
inline constexpr TypeSpec spec{.settings = SettingsTable<cardRow, playbackRow, captureRow>{"card"},
                               .banks = Banks<play, capture, fromCard, toCard>{}};
```

A bank handle is only its name, direction and reach, fixed at compile time; a name is one or more
of `[A-Za-z0-9_-]`, unique in the spec. A public port's whole name is `<bank>.<member>`, made in
one place (`portName`). Members come from settings, in the class's `ports()`.

## The class

One class, not final, made by the framework **once per configuration** (each change of settings,
restart or retry makes a new one). Its members are found by name whatever their access, so each
must be public and not overloaded, with exactly the signature below; anything else does not
compile.

| Member | Required | What |
|---|---|---|
| `static Members<spec> ports(const CheckedOf<spec>&)` | yes | Every bank once, with its members: `members(n)` for `1`..`n`, or `members("left", "right")`. Pure: it runs on any thread |
| `static std::expected<void, std::string> check(const CheckedOf<spec>&)` | no | A rule across settings, run at parse after the table's walk; its error is `invalid-settings` and never reaches `ear.conf` |
| `explicit Type(NodeOf<spec>&)` or `Type()` | one of them | `NodeOf` is valid for the instance's life: `format()`, `node()`, `handle()`, and `post(loop, work)` to a [loop](#loops)'s state |
| `Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>& s)` | yes | Acquire and construct only, on the main loop, then answer |
| `void start()` | no | Called once its code is driven (the node is then `running`); work that must wait for that is posted to a loop here |
| `void stop()` | no | Called once before each silence that follows a `start()` that returned; it posts its goodbye to its loops |
| `Changed configure(ChangeOf<spec>&, const CheckedOf<spec>& old, const CheckedOf<spec>& now)` | no | The change version: its signature is checked, but it is **never called yet**: every change of settings restarts the node, until in-place changes are built |

Settings are read by row: `s.get<playbackRow>()`. A misspelt row, a row of another table, or an
optional row read as a plain value does not compile.

### `configure` and `CreateOf`

| Call | What | Rules |
|---|---|---|
| `cx.format()`, `cx.node()` | The graph's rate (a `SampleRate`: frames and time convert through `FrameClock`, and its number, `hz()`, is for a library's edge, [engine.md](engine.md#dating)) and block size; the node's name | |
| `cx.use(code)` | The code: a struct with `process`, or `passThrough({bankPair(in, out), ...})` | Exactly once |
| `cx.soundCard(bank, SoundCardShape{.card})` | A sound card, an [engine companion](#engine-companions) on an inside bank: playback on an inside output, capture on an inside input (the bank's direction is the card's). Named `<node>.playback` or `<node>.capture`; made once the block is ready | Inside banks only (compile time); one companion per bank |
| `cx.rtpReceiver(bank, RtpReceiverShape{...})` | An RTP receiver, an [engine companion](#engine-companions) on an inside input bank, `<node>.rtp`; its `latencyMs` is latency beyond the bank | Inside input banks only (compile time); one companion per bank |
| `const Recording& r = cx.recording(RecordingRequest{path})` | A [recording](#recordings): a file beneath the recordings folder, claimed, then created or truncated off the main loop; give it to the sink's consumer, which reads `r.descriptor()` from `begin()` on | Nothing to answer: a refusal fails the node at its readiness step; one path asked for twice (however spelled) fails it `start-failed` |
| `cx.sink(bank, SinkShape{.frames = n}).consumedBy(std::make_unique<Consumer>(...))` | A sink on an inside output bank ([sinks](#sinks)) | Inside output banks only; exactly one consumer; one companion per bank |
| `SourceWriter& w = cx.source(bank, SourceShape{.frames = n}, clock)` | A source on an inside input bank ([sources](#sources)): what the type's own thread writes, played into the bank before the code; `clock`, a `std::unique_ptr<SourceClock>`, its own clock ([a source's clock](#a-sources-clock)), left out for a source with none | Inside input banks only (compile time); `0 < n <= maxSourceFrames` (2^24); one companion per bank |
| `cx.declareLatency(from, to, Latency{min, max})` | The code's own latency on the path from an input bank to an output bank, in frames: carried as a path through the block, so it counts in both banks' told latency ([latency](#latency)) | Once per path (a mixer declares each of its inputs' paths into one output); with a pass-through, one of its pairs; a plan that does not fit the ports fails the node `start-failed` |
| `cx.loop(handle, std::make_unique<State>(...))` | A [loop](#loops)'s state, constructed here with no I/O and no threads, owned by the framework from now on | Exactly once for a loop declared with a state type (compile time: never for one declared without) |
| `Loop& l = cx.executor(handle)` | A loop as an executor, for a state to keep: `post`, timers and watches | Valid until the configuration is freed; what it is sent before the node is configured is staged |
| `cx.post(handle, [](State& s) { ... })` | Work on the loop with its state (`[] { ... }` for a loop with none), staged | A function of another loop's state does not compile |
| `Store& s = cx.store(handle)` | A [store](#stores), for a loop's state or a consumer to keep | Valid until the configuration is freed; readable once the node is configured; a store the spec lacks does not compile |
| `cx.claim(claim::TcpListen(p))`, `cx.claim(claim::Udp(p))` | A port the type's own thread binds, a [claim](#claims): judged at the first readiness step and probed, so a port another node or program holds fails the node before anything is opened | 1 to 65535; held until teardown step 5 |

Then answer `Configured::applied()`, or `Configured::failed(detail)` for what only the type can see
(the node fails `start-failed: <detail>`). A `CreateOf` is valid only inside the `configure` it is
handed to. What a type needs from outside it acquires here; a type's factory takes no arguments.

Each call is served by the host's adapter of its kind (`ResourceAdapters`, one member per kind,
`node_host.hpp`). ear's host has every kind; a host without one (a test's) refuses the call as an
adapter's refusal is: the node fails `start-failed: configure threw: no <kind> in this host`
(`sinks`, `sources`, `sound cards`, `RTP receivers`, `loops`, `stores`); a recording fails it
`start-failed: cannot open <path>: no recordings folder`. A host with no port probe judges a port
[claim](#claims) between its nodes only.

### The node handle

`NodeOf::handle()` gives the configuration's `NodeHandle` (`ear/core/node_handle.hpp`), the type's
one way out from its own threads. The instance, a thread it owns or its sink consumer may keep it
and call it until the configuration's teardown ended, from any of those threads but never from the
realtime thread (`process`): a call takes brief locks (the note rate's, the main loop's queue, a
log sink's) and never waits on the main loop.

| Call | What | Order and rate |
|---|---|---|
| `node()` | The node's name | |
| `say(text)` | A note, `node <n> says <text>` on standard output (a `note` record, [output-records.md](output-records.md)). Never a state | Crosses to the main loop in the order said, so it is ordered with the node's states. Paced per configuration: 32 at once, then one a second; the framework says `notes-dropped <n>`, the count held back, as the configuration's own note just before the next that passes. Judged on the thread that says, so a note held back is never queued |
| `lost(detail)` | The node fails, `start-failed: <detail>` before it first rested `running` or `stopped`, `lost: <detail>` after; ignored once the configuration's teardown began | In order with notes; never held back by their rate |
| `log(entry)`, `debug(text)`, `info(text)`, `warn(text)`, `error(text)` | A `log` record ([output-records.md](output-records.md)): the framework sets its `node`, and an empty `source` becomes the type's name (a library's own lines give its name, and a `topic`) | Straight to the log sinks on the calling thread, unordered with notes and states, as captured output is; paced per node by standard output's encoder |

The handle is the configuration's: a stop and a start keep it (and its note rate), a restart, a
change of settings or a failure ends it, and the next configuration has its own. What it carries
lands only on the configuration that made it: once that configuration has ended (closed, failed
to start, or leaked), calls through it are dropped, so a thread of an old configuration that is
still finishing never reaches a newer one. One exception is narrow: a `log` that races the end
itself may still write its one line, since it checks the configuration, then writes, and the end
does not wait for it; a note or a loss that races it is dropped on the main loop. A note said during the teardown, before the
configuration ended, is still heard, so a goodbye is. NodeHost implements the handle on the
configuration's own channel, the one queue to the main loop every cross-thread report of the
configuration rides ([architecture.md](architecture.md#teardown-by-dependency)); no type keeps a
generation of its own.

## The code

```cpp
struct MyCode {
	void prepare(const AudioFormat&);                       // optional; main loop, before the first process; may allocate
	void process(Io<spec> io, const GraphCycle& cycle) noexcept;   // required; the realtime thread
	void release() noexcept;                                // optional; main loop, after the last process
};
```

- `io[bank]` is the bank's members, each a span of exactly `io.frames()` samples: an output bank's
  to write (`std::span<float>`), an input bank's to read (`std::span<const float>`). Only the spec's
  own banks compile, and writing into an input does not. An unlinked input reads zeros.
- **Write every output sample, every cycle.** The harness fills outputs with NaN first and counts
  what was left.
- **Realtime rules:** no locks, no allocation, no file or network access, no waiting, no system
  call that can block. To reach another thread, use a lock-free mailbox or ring the type owns as a
  member of its instance, made in `configure` before the code drives and freed after it.
- `GraphCycle` gives the cycle's start on `CLOCK_MONOTONIC` (`startNs`) and `CLOCK_MONOTONIC_RAW`
  (`startRawNs`), the driver's `position`, `rateDiff` and `driverId`, `nextNs`, the cycle's `facts`
  (what was lost before it and why it is a discontinuity: [below](#lost-cycles)), and
  `downstreamOf(output)`, the latency downstream of an output port. Never read a clock yourself
  ([engine.md](engine.md#graphcycle)), and never work a loss out from `position`: read the facts.
- A code written against another type's spec compiles but fails the node at `use`.
- **The pass-through.** `passThrough({bankPair(fromBank, toBank), ...})` copies each input bank's
  ports to an output bank's, member for member, and silences every output no pair names. Banks of
  a pair must be the same size, an output bank written once. `card` and `rtp` write no audio code
  of their own: they pass between their inside and public banks.

## Engine companions

An engine companion is an engine node of its own that a configuration asks for on one of its
inside banks: a sound card (`cx.soundCard`) or an RTP receiver (`cx.rtpReceiver`). The type gives
the bank's handle and a shape (`ear/core/companions.hpp`), never a direction or a port: a card on
an inside output bank plays it, one on an inside input bank captures into it, and a receiver feeds
an inside input bank, one channel per port of the bank. Every member of a shape is checked by the
type's settings table first (a `CardId`, an `RtpAddress`, closed enums, ranged numbers), so nothing
unparsed reaches the engine.

```cpp
cx.soundCard(toCard, SoundCardShape{.card = s.get<cardRow>()});    // playback: to_card is an inside output
cx.rtpReceiver(fromRtp, RtpReceiverShape{.address = s.get<addressRow>(), /* port, encoding, rate, */
                                         .latencyMs = s.get<latencyRow>()});
cx.use(passThrough({bankPair(play, toCard), bankPair(fromRtp, stream)}));
```

- The framework makes each one once the node's block is ready, in the order asked, and joins its
  channels to the bank's ports ([engine.md](engine.md#engine-companions)). The node is `running`
  only once every companion is ready; one that is lost, before or after, fails the node
  `engine-refused: <detail>`, and the teardown removes it after the code's silence. One not ready
  within its kind's deadline (a card 10 s, a receiver 5 s, from the request) fails it too, with the
  kind's text ([engine.md](engine.md#readiness-deadlines)); a type writes nothing for it.
- A playback card's bank fades with the public outputs; a capture card's or receiver's input is
  never faded.
- An inside bank has at most one companion of any kind (an engine companion, a sink, a source);
  a second is a broken rule (`a second companion on bank <bank>`).
- A receiver's `latencyMs` is latency beyond its bank, which the framework adds to the latency
  plan ([latency](#latency)); a type declares none for it. A card's latency arrives through its
  links.

## Sinks

A sink takes audio off the realtime thread. The type asks for one on an inside output bank:

```cpp
cx.sink(writer, SinkShape{.frames = static_cast<std::uint32_t>(FrameClock(cx.format().rate).frames(2s))})
	.consumedBy(std::make_unique<WavWriter>(...));
cx.use(passThrough({bankPair(record, writer)}));
```

- Each cycle the code ran, after the code, the block interleaves the bank's frames into the sink's
  lock-free ring with a stamp (the cycle's position, both clocks, its causes and lost frames, the
  upstream latency) beside them, and wakes the sink's own thread.
- The `SinkConsumer` is called on the sink's own thread only, one call at a time: `begin()` once,
  before the node is `configured` (open what it writes to; a failure is `handle().lost(detail)`,
  which fails the node `start-failed`; one that has not returned within 5 s of configure's return
  fails it `start-failed: the sink <bank>'s consumer did not begin within 5 s`,
  [engine.md](engine.md#readiness-deadlines)); `consume(const Chunk&)` for each chunk in order;
  `finish()` once at the teardown, after the last chunk. Each may block on I/O. A call that throws
  is a bug the framework catches on the sink's thread, a fact about the node as a hook's throw is
  ([below](#rules-the-framework-enforces)): after a `begin()` that threw nothing more is called,
  after a `consume` that threw no further chunk is handed over, and `finish()` still follows a
  `begin()` that returned.
- A sink takes exactly the cycles its code drove: a stop's silence ends them, a start begins them
  again; a stop closes nothing. `Chunk::causes` are the cycle's ([below](#lost-cycles); `discontinuity()`
  when any): `drive` on the first chunk after any drive, `catchUp` after cycles the graph lost and
  caught up, and so on. `Chunk::lostFrames` is what a catch-up lost before the chunk (its
  `jumpFrames`), 0 otherwise, so a consumer may write the loss as silence or say it; a stop's gap is
  a `drive` with nothing lost stated, and shows in `Chunk::position`. A chunk split at the ring's end
  carries its causes and lost frames on its first call only.
- `Chunk::upstreamFrames` is the most capture latency the block told on the sink's bank in the
  cycle the chunk was taken: what lies upstream of the input banks paired with it, plus what the
  configuration declared on it ([engine.md](engine.md#sinks)). So the chunk's first frame entered the
  graph at `FrameClock(rate).enteredRawNs(startRawNs, 0, {0, upstreamFrames})` (`FrameClock`, in
  `ear/core/latency.hpp`, is the one place frames become time: [engine.md](engine.md#dating)), for
  each call of a chunk split at the ring's end too (the second call's start times are its own first
  frame's). A type converts frames and time through it too. It is the most of each part,
  as PipeWire combines latencies, so an upper bound when the bank's paths differ. A consumer that
  does not date audio ignores it (`file`'s `WavWriter` does).
- The frame ring and the stamp ring drop together (no frame without its stamp); dropped frames are
  counted (readable at any time by the framework), shown in the node's companion list, said as
  `ear.sink-dropped` while they grow ([engine.md](engine.md#reporting-the-counters)) and summed into
  the node's `dropped=` at removal.
- These rules are core's `SinkRing` (`ear/core/sink_ring.hpp`, each chunk cut by `cutChunk` in
  `ear/core/sink.hpp`), which the engine's sink and the harness's both run, so a type's tests get the
  calls the engine makes, split chunks included.
- A sink's bank is never faded, so a recording keeps exact samples; a node whose only outputs are
  sinks' banks is silenced at once, with no fade before.
- The consumer is owned by its sink and freed at teardown step 5 after the code and before the
  instance, so it may keep a reference into the instance (or what the instance owns) and use it in
  `finish()` and its destructor ([architecture.md](architecture.md#teardown-by-dependency)).

## Sources

A source brings audio onto the realtime thread from an ordinary one (a decoder, a network library's
thread). The type asks for one on an inside input bank and passes it on:

```cpp
SourceWriter& writer = cx.source(fromServer, SourceShape{.frames = 2048});
cx.use(passThrough({bankPair(fromServer, from)}));
// the type's thread, later: writer.write(interleaved, frames, 20ms); writer.idle(20ms); writer.clear();
```

The source is made at once and is ready at once (nothing to wait for). Each cycle the code runs,
before the code, the block plays the source's ring into the bank, so the code reads the bank as it
would a linked input. Its rules, all in core's `SourceRing` (`ear/core/source_ring.hpp`), which the
engine and the harness both run, so a type's tests and the engine never disagree:

- **The writer.** `write(interleaved, frames, patience)` takes what fits (whole frames, interleaved
  over the bank's members) and, with no room, waits up to `patience` for the block to play some; it
  returns the frames taken. One writing thread at a time. `idle(patience)` says the writer has
  nothing to give (an empty ring is then no shortfall) and waits `patience`; the next write ends the
  idle. `clear()` (any ordinary thread) drops what was written so far at the next cycle. A writer
  starts idle. The ring holds `frames` rounded up to a power of two (`capacityFrames()`).
- **It keeps no time.** The ring plays what it holds, in order, on each tick, and nothing else: it
  skips nothing of its own, owes nothing and counts nothing played
  ([the timing model](architecture.md#the-timing-model), rule 1). After a stall the graph caught up
  (`catchUp`), a pause, a reposition or a driver change, a source with no clock plays its ring on
  from where it was; the stall's time is time the writer did not need to fill. A source whose audio
  has a clock of its own (a server's timeline, a network sender's) gives the source that clock, and
  the clock decides what to skip ([below](#a-sources-clock)). `white` needs none: it makes its audio
  on the tick.
- **It fades at its edges, with no lookahead**, so the latency it adds stays `[0, capacity]`: audio
  that starts or comes back fades in over 5 ms; at an underrun (the ring dry) it decays from the
  last frame it played to zero over 5 ms; after `clear()` it plays the next 5 ms of what was
  written before the clear, faded to zero, drops the rest (counted dropped), and only then fades
  the new audio in. A source's bank is never faded by the drive: its fades are its own.
- **The shortfall.** Frames played as zeros (or as an underrun's decay) while the writer was not
  idle are counted, readable at any time (`Source::shortfallFrames()`), shown in the node's
  companion list and said as `ear.source-shortfall` while it grows. A skip that leaves the ring
  dry is a shortfall too, if the writer is not idle.
- **A stop** unbinds the source with the code; once the silence completed, what its ring holds is
  dropped (counted, never played), so a start never plays audio written before it. Meanwhile a
  write waits as at a full ring; the first cycle after the next start lets it go, with no shortfall
  counted for that cycle. When a stop or a teardown begins, before the type's `stop()`, the writer
  counts as idle, so a `stop()` that stops the writer's thread leaves no shortfall in the fade and
  the silence. A `stop()` must not call `write` or `idle` itself: they wait, and `stop()` runs on
  the main loop, which they would sleep.
- **Closing.** At the teardown's first step the framework closes the writer: a `write` or `idle`
  already waiting returns at once, and one that begins after the close accepts nothing and sleeps
  its whole patience (a library thread calling in a loop never spins). The writer stays valid
  until the configuration is freed, after the instance, so a thread the instance owns may hold it.
- **Latency.** The ring's capacity is latency beyond the bank, `Latency{0, capacity}`, in the
  configuration's plan, added in series to what the bank's peers report (none, for a source's bank)
  ([engine.md](engine.md#latency-per-bank-pair)), so the public bank paired with it tells it upstream.
- One companion per inside bank: a source on a bank that has a receiver or another source is a
  rule broken (`start-failed`), and so is `frames == 0` or a source on any other bank (which does
  not compile).

No shipped type uses a source yet: `sendspin`'s feed moves onto one when its node moves off the old
interface.

### A source's clock

A source whose audio keeps time on a clock of its own reconciles that clock with the tick inside
the node ([the timing model](architecture.md#the-timing-model), rule 3), through a `SourceClock`
(`ear/core/source.hpp`) it hands the source when it asks for it:

```cpp
auto clock = std::make_unique<ServerClock>(/* what it reports to */);
ServerClock& mine = *clock; // kept: the source frees it after the instance and its threads
SourceWriter& writer = cx.source(fromServer, SourceShape{.frames = 2048}, std::move(clock));
```

Each tick, the one play rule (`SourceRing::play(bank, tick, clock)`, the same code in the block and
the harness) runs in this order:

1. A clear asked for is applied: the ended stream's next 5 ms queued to play faded, the rest dropped.
2. `clock.skipBefore(tick, held)` is asked how much of the current stream to drop unplayed:
   `held.frames` is what the ring holds of it, `held.clearing` whether this tick applied a clear.
   The ring drops **min(answer, held.frames)**, and **nothing on a clearing tick** (a clear ends the
   stream: nothing of the next is skipped for the old one's lost time). Any answer is allowed; the
   clock reads the tick's facts (`tick.cycle.facts`: a `catchUp` and its `jumpFrames`,
   [lost cycles](#lost-cycles)) or its own reckoning (its network fell behind) and decides.
3. The tick's frames are played: the ended stream's faded 5 ms, the current stream's frames (faded
   in where audio came back), then an underrun's decay and zeros.
4. `clock.played(tick, played)` is told exactly what the tick did, a `SourcePlayed`: `skipped` (of
   the current stream, at the clock's word), `read` (of the current stream, played), `ended` (of a
   stream a clear ended, never the current one's) and `zeros` (zeros, a decay or a clear's fade past
   its frames); `read + ended + zeros` is the tick's frames. A clock counting what its library
   wrote counts `skipped + read`, never `ended` or `zeros`.
5. If `played` returned true, the source's **clock wake** (`writer.clockWake()`) is rung, at most
   once until answered: its `descriptor()` is readable while rung, and its one watcher calls
   `answer()` before it looks. It is never rung for a source with no clock.

`SourceTick` carries the tick whole (`cycle`, its `GraphCycle`), the ring frames it plays
(`frames`), the ring's rate (`rate`, the graph frequency), and `playback`: the most playback latency
the block told on the source's bank, for each output it is paired with what lies downstream of it
(a following card's buffer) plus what the configuration declared on that output with
`declareLatency`, in graph frames. A clock dates what played from the tick's start, its frames and
that latency (`FrameClock::heardRawNs`, [engine.md](engine.md#dating)); what the date means (heard
by a device, or only passed by the graph's clock: a cycle cut short after the block loses frames it
played, and no node can see that) is the clock's to know.

Both calls run on the realtime thread, never concurrently: a clock never blocks, locks, allocates
or makes a system call. It reports to its type's threads in a lock-free mailbox it owns, and they
read it through the reference the type kept; the source owns the clock and frees it with itself at
teardown step 5, after the instance and every thread it owns, so that reference never dangles. A
source with no clock (`cx.source(bank, shape)`) skips nothing and is never asked.

## Loops

A type's own threads are its loops: serial executors for its work off the realtime thread and the
main loop, declared in the spec, each with its state, alive for the node's whole configuration
(`ear/core/loop.hpp`). A loop is a **strand** on the framework's pool (its handlers never block) or
**dedicated**, on a thread of its own named `ear-<name> <node>` (cut to 15 bytes; its handlers may
block: a library's poll, a disk write, a source write with patience). A type writes no thread, poll
loop, eventfd or stop flag.

```cpp
class Worker;   // the loop's state: the library's client, its timers
inline constexpr auto worker = loop::dedicated<"worker", Worker>; // 1 to 8 of [a-z0-9-], unique in the spec
inline constexpr auto helper = loop::strand<"helper">;            // no state: an executor other states use
inline constexpr TypeSpec spec{.settings = ..., .banks = Banks<...>{}, .loops = Loops<worker, helper>{}};

Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) {
	cx.loop(worker, std::make_unique<Worker>(node_.handle(), cx.executor(worker), cx.executor(helper)));
	...
}
void start() { node_.post(worker, [](Worker& w) { w.begin(); }); } // arms its timers, starts the library
void stop() { node_.post(worker, [](Worker& w) { w.end(); }); }    // the goodbye
```

| On a `Loop&` (`cx.executor(handle)`) | What |
|---|---|
| `post(work)` | Any thread; runs `work` on the loop, one handler at a time, in post order, never inside the call |
| `after(delay, due)`, `every(period, due)` | A timer (`CLOCK_MONOTONIC`); `every` keeps a fixed rate, and the ticks a slow handler missed run once |
| `onWake(wake, rung)` | `rung` each time `wake` rings (a source's clock wake, `SourceWriter::clockWake()`); the loop answers the wake just before the handler, so a ring during it runs it again, and a wake rung before it was watched runs it once |
| `onReadable(fd, readable)` | `readable` each time the descriptor (the caller's, outliving the watch) is readable |
| `inLoop()`, `kind()` | Whether the calling thread runs this loop's handler; its kind |

Each arming call returns a `std::unique_ptr<Timer>`: destroying it cancels the timer or watch, and
its handler never runs after the destructor returns. It is destroyed on its loop's thread (a state
keeps it as a member), or on the main loop while the node is not yet configured.

- **The state.** Given in `configure`, constructed on the main loop with no I/O and no threads; from
  then on the instance holds no reference to it, and reaches it only by a post (`NodeOf::post`,
  `cx.post`), typed by the handle, so a function of another loop's state does not compile. A stop
  and a start keep it (and its loops); a restart, a change of settings or a failure destroys it.
- **Staging.** Everything sent to a loop before the node is configured (a post from `configure` or
  from a state's constructor, a timer or watch armed there) is staged on the main loop. Once the
  node is configured (its loops running, every companion ready, its code prepared) the staged items
  run on their loops in the order sent, ahead of anything sent later; a staged timer counts from
  there. If the configuration is torn down first, they are destroyed unrun. So no handler runs
  before every resource the node asked for is ready. Work that must wait for the code to drive is
  posted by `start()`.
- **When loops start.** Each loop is an item of the configuration's readiness list, at the step
  before its companions ([engine.md](engine.md#readiness-deadlines)): the framework starts every
  loop once configure answered applied, and makes the engine companions and begins the sinks'
  consumers only once they run, so a loop that cannot start costs nothing else. It fails the node
  `start-failed: cannot start loop <name>: <why>`; one not yet running is `waitingFor` `loop`.
- **Teardown.** At step 1 each loop refuses work from outside the node (slice 5's actions); the
  node's own posts go on. At step 4, once the sinks drained, each loop runs what was posted (the
  goodbye `stop()` posted), then destroys its state on its own thread (where its destructor may join
  the threads its library owns); once every loop has, each closes. A state whose loop never started
  is destroyed on the main loop, before the instance it may read. The close deadline (10 s) covers
  it: a loop that never drains leaks the configuration, `ear.close-overdue` counting it open
  ([architecture.md](architecture.md#teardown-by-dependency)).
- **Sources left armed.** A state destroys the timers and watches it armed, before or in its
  destructor; one armed on another loop it destroys by a post to that loop. The framework counts
  every one whose handle lives; one still armed when the loops close is a type bug: its handler
  never runs once its loop's state is gone (between the state's destruction and the close it may
  still come due, and is skipped), its loop cancels it at the close, the node says `ear.loop-source-left`, and the harness fails the call that tore the
  node down.
- **Threads.** A handler runs on its loop's thread, one at a time per loop. Nothing here is
  realtime. A strand must not block: `SourceWriter::write` and `idle` assert in debug builds that
  their caller is not a strand (`onStrand()`). A strand handler past 100 ms is `ear.loop-slow`
  (once per loop and configuration); past 1 s the pool starts a replacement thread (at most 16), so
  one stuck strand delays no other node's. A dedicated loop is never judged.
- **A throw.** A handler that throws is caught by its loop: the node fails as a hook that throws
  does, `loop <name>'s handler threw: <what>` (`start-failed`, or `lost` once rested; during a
  teardown a `log` record), and the loop runs on.

## Stores

A store is a small file that belongs to one node and outlives its configurations (a private key,
a stream's identity), declared in the spec, named by the framework, read and judged before any of
the node's loop work runs, kept no wider than its mode, and replaced only if unchanged
(`ear/core/store.hpp`).

```cpp
std::string newSsrc();                                          // the text a missing file is created with
std::optional<std::string> whyNotSsrc(std::string_view text);   // why a text is not trusted, or nothing
inline constexpr auto ssrcFile =
	store::file<"ssrc">({.mode = 0644, .maxBytes = 64, .initial = &newSsrc, .distrust = &whyNotSsrc});
inline constexpr TypeSpec spec{.settings = ..., .banks = Banks<...>{}, .stores = Stores<ssrcFile>{},
                               .loops = Loops<control>{}};

Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) {
	cx.loop(control, std::make_unique<Sender>(node_.handle(), cx.store(ssrcFile), cx.executor(control)));
	...
}
```

| `StoreRule` | What |
|---|---|
| `mode` | The most the file's mode may be (default `0600`): a wider file is narrowed, never refused |
| `maxBytes` | Required, at most 1 MiB: a larger file is neither read nor written |
| `initial` | The text a missing file is created with; left out, a missing file stays missing (`text()` is empty) |
| `distrust` | Why a file's text is not trusted, or nothing; left out, any text is. It runs on the I/O thread, so it reads nothing but its argument |

| On a `Store&` | What |
|---|---|
| `fileName()` | `<type>-<node>.<suffix>` |
| `text()` | Any thread: what the file held when it was opened (or was created with), then the text of the last write that committed |
| `replace(text)` | Blocking, at most 5 s, so from a dedicated loop or a library's thread, never a strand (asserted in debug builds), the main loop (a `std::logic_error`, so the node fails: **a hook uses `replaceLater`**) or `process`. Its outcome is a `StoreWritten`: done, or a `StoreWriteError` of kind `refused` or `unconfirmed` (below) |
| `replaceLater(text, loop, done)` | The same without waiting, from any thread, a hook's included; `done(outcome)` runs on `loop`, one of the node's own, with the exact outcome (never `unconfirmed`), or never once that loop closed |

- **Name and place.** A store's file is `<type>-<node>.<suffix>` in the graph file folder
  (`--config-dir`): its suffix is 1 to 16 of `[a-z0-9-]`, unique in the spec. A spec gives its
  members in order, `.stores` before `.loops`, and may leave out either.
- **Open.** Every declared store, taken by `configure` or not, is an item of the configuration's
  readiness list at step `opens`, before its loops ([engine.md](engine.md#readiness-deadlines)): one
  job on the I/O thread reads the file (no symlink anywhere on the path, at most `maxBytes`) and
  narrows it to the mode on the same descriptor, **before** its text is judged (a file refused for
  its text may still hold a secret, and a file renamed over the name meanwhile is never the one
  judged while another is narrowed), then judges it by `distrust`; a missing file is created with `initial()` at exactly
  the mode. So a store that fails starts no loop. It fails the node `start-failed: <file>: <why>`
  (`distrust`'s reason, or the disk's: `rtpsend-porch.ssrc: malformed: not eight hex digits`), the file left as it
  was: a distrusted or unreadable file is never replaced by a new one. An open past 5 s fails it
  `start-failed: <file>: not opened within 5 s`; one not yet open is `waitingFor` `store`, by its
  file.
- **Reading.** The node is configured only once every store is trusted, and staged loop work runs
  only then, so a loop's handler, `start()` and a sink consumer's `begin` read it freely. Read or
  written before its open completed (in `configure`) is a `std::logic_error`: the node fails.
- **Writing.** Every write is one job on the I/O thread, in order, each against the text the one
  before committed: the file narrowed first (a mode widened since is never kept), then the new
  text swapped in (a staging file renamed over it) only if the file still holds `text()`. A
  `refused` write did not commit and the file is as it was; its detail names the file:
  `<file>: changed by something else` (someone else's text stands), `<file>: larger than <n>
  bytes`, `<file>: not written: not begun within 5 s` (it never runs), or the disk's. An
  `unconfirmed` one was under way on the I/O thread when `replace`'s 5 s passed: it may still land,
  so it is **never** "not written"; compare `text()`, which shows it once it landed, or write with
  `replaceLater`, whose outcome is exact. A failed write fails nothing: telling it (a note) is the
  type's.
- **Its life.** A stop and a start keep it; a restart, a change of settings or a failure opens it
  again in the next configuration. It is usable until teardown step 4 and freed at step 5. An open
  or a write still on the I/O thread when its configuration ended tells nothing, and what it held
  is released on the I/O thread.
- **Boot.** Before any node exists, every file in the folder that is one of a declared store's
  (`<type>-*.<suffix>`, whatever its node, so one no longer in the graph too) and each one's
  staging file a crash left is narrowed to its store's mode; what cannot be is a `log` record at
  `warn` from the type.
- **Faults.** A bug of the stores' adapter (a throw from the disk's part, on the I/O thread) fails
  the node `lost`, `internal error in StoreAdapter::<call>: <what>`, said first in an
  `ear.internal-error` record ([architecture.md](architecture.md#faults-by-owner)).

No shipped type has a store yet: `sendspin`'s key file is its own until it moves onto one.

## Recordings

A recording is a file beneath the recordings folder (`--recordings-dir`) that one configuration
writes, through its sink's consumer (`ear/core/recording.hpp`). `file` is its user.

```cpp
Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>& s) {
	const Recording& recording = cx.recording(RecordingRequest{s.get<pathRow>()});
	cx.sink(writer, SinkShape{.frames = ...}).consumedBy(std::make_unique<WavWriter>(recording, ...));
	...
}
```

| On a `Recording&` | What |
|---|---|
| `path()` | The path as asked, relative to the recordings folder |
| `descriptor()` | Any thread, once opened: the file's descriptor, the framework's (never close it: libsndfile takes it with `sf_open_fd(fd, mode, &info, SF_FALSE)`). Read before its open completed (in `configure`) is a `std::logic_error`: the node fails |

- **The path rule.** A path is relative to the recordings folder, with no `..` component: one home,
  `recordingPathRefused` (`ear/core/recording.hpp`), which `cx.recording` enforces whatever the type checked. A path
  reaches a type from its settings, so a refused one is the node's failure, not the type's bug: `start-failed: cannot
  open ../take.wav: must be relative to the recordings folder, without ".."`, at configure's return, nothing claimed or
  opened. A type whose setting is a path checks it at parse with the same function, so the edit is refused as
  `invalid-settings` before any node starts (`file`'s `path: must be relative to the recordings folder, without
  ".."`).
- **Claimed first.** Its path is a [claim](#claims), `recording:<path>` with the path's `.` and empty components
  removed (`./take//1.wav` is `recording:take/1.wav`). A symlink inside the folder is not followed, so two spellings
  that reach one file through one are two claims and the second open truncates the first one's file. Telling them
  apart would need the file's identity (`st_dev`, `st_ino`), which only an open can read: either after a truncating
  open, too late, or by an open in two phases (open, compare among the open recordings on the I/O thread, then
  truncate) that would refuse where a claim waits (a removal and an addition in one edit) and owe nothing to a leaked
  holder's late close. The recordings folder is ear's, written only by its `file` nodes, so such a symlink is the
  operator's own; ear leaves it to them. A path another node's configuration holds is refused before anything is opened, so
  the other node's file is never truncated: `start-failed: cannot open take1.wav: held by node a`; one held by a
  configuration that is closing (a removal and an addition on one path in one edit) waits for its last step. One path
  asked for twice in one configuration, however spelled, is the type's bug: `start-failed: configure threw:
  recording:take1.wav claimed twice`.
- **Opened off the main loop.** Once granted, it is an item of the readiness list at step `opens`, after the stores
  ([engine.md](engine.md#readiness-deadlines)): one job on the I/O thread creates or truncates it, beneath the folder
  only (`openat2` with `RESOLVE_BENEATH`: no `..`, no symlink out, no subfolder made). A refusal fails the node
  `start-failed: cannot open <path>: <why>` (the folder's reason); an open past 5 s, `start-failed: cannot open <path>:
  not opened within 5 s`; one not yet open is `waitingFor` `recording`, by its path. A sink's consumer begins only at
  step `companions`, after it, so `begin()` reads an open descriptor and writes its header before the node is
  configured. A host with no recordings folder fails the node at configure's return, `start-failed: cannot open
  <path>: no recordings folder`.
- **Its life.** A stop and a start keep it open; a restart, a change of settings or a failure opens it again (and so
  truncates it) in the next configuration. It is freed at teardown step 5, after the sinks that write it, the instance
  and the stores, and closed there on the I/O thread, before its claim is released; one path's close and its next
  open are jobs on that one thread in order, so the next configuration's truncation comes after the close. An open
  still on the I/O thread when its configuration ended tells nothing, and its file is closed on the I/O thread. A
  leaked configuration keeps its recordings and their claims until it closes late (its sink may still write the
  file); then each one's close is posted to the I/O thread before its claims are released, so a node its claims
  refused, started again then, opens the path after the close.
- **Faults.** A bug of the recordings' adapter (its `std::logic_error`, or no file made) fails the node `lost`,
  `internal error in RecordingAdapter::open: <what>`, said first in an `ear.internal-error` record whose `where` is
  `the I/O job open recording <path>` ([architecture.md](architecture.md#faults-by-owner)); any other throw (no
  descriptor, no memory) is a refusal, as the folder's reason is.

## Claims

A claim is something ear-wide that one configuration at a time may hold: a listening TCP port, a
UDP port, a store's file, and a [recording](#recordings)'s path (`ear/core/claims.hpp`). Each is
keyed by its kind and value (`tcp:8928`, `udp:5004`, `store:sendspin-kitchen.key`,
`recording:take1.wav`), and a key has one holder across every type.

```cpp
Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>& s) {
	cx.claim(claim::TcpListen(s.get<portRow>())); // a port the type's own thread will listen on
	...
}
```

- **Where claims come from.** A type claims only the ports its own threads bind: `cx.claim(claim::TcpListen(p))` or
  `cx.claim(claim::Udp(p))`, each taking a row's checked number (outside 1 to 65535 is the type's bug, a
  `std::logic_error`). The framework claims the rest: every store the spec declares (its file), every recording
  (its path), and what each engine companion holds by its shape (`claimsOf`, beside `moduleRunDelay` in `companions.hpp`): an RTP receiver its UDP
  port, a sound card nothing yet. So no shape's field is named outside its shape's rule.
- **Judged first.** Every claim of a configuration is an item of its readiness list at step `claims`, before any
  store is opened, loop started or companion made ([engine.md](engine.md#readiness-deadlines)). Against the key's
  holder:

  | Holder | Outcome | The node fails `start-failed:` |
  |---|---|---|
  | none | granted; a port then probed (below) | the kernel's reason: `cannot bind UDP port 5004: Address already in use` |
  | a configuration that is not going (a node configuring, running or stopped) | refused at once | `cannot bind UDP port 5004: held by node fpp`; `cannot open take1.wav: held by node rec` |
  | a configuration that is going (its teardown began: a restart's, a failure's, a removal's or a rename's old one) | waits, said once by `ear.claim-wait` and `waitingFor` `claim`; granted at that configuration's step 5 | after 15 s: `... held by node fpp, which did not let go within 15 s` |
  | a configuration leaked at the close deadline: a port | abandoned to the claimant, probed afresh | the kernel's reason, owed to the leaked configuration (below) |
  | a configuration leaked at the close deadline: a store or recording | refused, owed to the leaked configuration | `<file>: held by node porch's earlier configuration, which did not close; it starts when that closes` |
  | a configuration leaked on an overdue barrier (the data loop wedged), which never closes: a port | abandoned, probed afresh, owed to nobody | the kernel's reason |
  | the same: a store or recording | refused, owed to nobody | `<file>: held by node porch's earlier configuration, which the wedged data loop keeps; it starts once ear restarts` |

  A disabled node holds nothing (it has no configuration). A node never waits on its own old configuration, whose
  step 5 comes before its next configure; two nodes whose restarts swap ports both run. A port claimed twice in one
  configuration (by `cx.claim` twice, or by `cx.claim` and an engine companion's shape) is the type's bug: the call
  throws `std::logic_error`, and the node fails `start-failed: configure threw: udp:5004 claimed twice` even when the
  type catches it. The framework's own store claims are held once each; a recording's path asked for twice is
  refused likewise ([recordings](#recordings)).
- **The probe.** Once the registry grants a port, it is bound once by a non-blocking socket on the main loop and
  closed, as its holder will bind it. A UDP port on every address with neither `SO_REUSEADDR` nor `SO_REUSEPORT`, so
  a holder that set it (PipeWire's RTP receiver, [engine.md](engine.md#rtp-receivers)) still refuses; a UDP port on a
  multicast group (an RTP receiver's, which a `Claim` carries beside the port, never in its key) at `<group>:<port>`
  with `SO_REUSEADDR`, as the receiver binds it, so another program sharing the port that way (a monitor listening to
  the group) passes and one holding it alone refuses; a TCP port with `SO_REUSEADDR`, as a listener binds,
  so a `TIME_WAIT` passes. The probe runs behind the turn's flush, so an engine companion a teardown has just queued
  for destruction (a restarted `rtp` node's own receiver) has let go of the port first. A program taking the port just
  after the probe is not caught. A host with no probe (a test's) judges ports between its nodes only.
- **Held, then released.** A stop and a start keep every claim. Teardown step 1 marks them going; step 5 releases them
  last, and whoever waited is judged again in arrival order (the first granted, the next refused by it); a waiter
  torn down before its grant leaves the queue at once.
- **A leaked configuration** ([architecture.md](architecture.md#teardown-by-dependency)) abandons its ports, whose
  holder it can no longer vouch for, and keeps its stores and recordings, whose files its threads may still write. It
  lists every node its claims refuse (a store kept, or a fresh probe of an abandoned port that failed). When it closes
  late its claims are released, `ear.close-late` adds `; its claims are released`, and each node it refused is
  started again, once, as `node.restart` would, if that refusal is still what the node rests failed on.
- **Not claimed:** an mDNS name (`sendspin`'s carries its key file's client id, so two nodes cannot share one), a
  multicast group apart from its port (two `rtp` nodes on one port are refused even for two groups, until a multicast
  test exists), and a sound card's device (until a card renamed in one plan is shown on the hardware to open its device
  before the old node closed it).

`sendspin`'s listening port is still its own claim, judged among its own nodes only, until it moves onto
`cx.claim`.

## Latency

A type declares only **its own** latency: how long its code holds audio between an input bank and
an output bank (`sendspin`'s feed). The framework carries everything else, a companion's included
(an RTP receiver's buffer, a source's ring, as latency beyond its bank):
a pass-through's pair forwards what lies beyond it, each block tells its linked ports, and the code
reads the latency downstream of its outputs in `GraphCycle`
([engine.md](engine.md#latency-per-bank-pair)).

`cx.declareLatency(from, to, delay)` makes `from` to `to` a path the block carries, whether the code
is a pass-through (which must copy that pair) or the type's own, with `delay` on it both ways, once
per path (a mixer declares each of its inputs' paths into one output): what lies beyond `to` plus the
delay is what `from` tells upstream (so a source on `from` tells its clock enough playback latency), and what
lies beyond `from` plus the delay is what `to` tells downstream (so a sink on `to` stamps its chunks
early enough). The framework works the configuration's plan out at configure's return; one that
does not fit the node's ports fails it `start-failed` with `configure gave a latency plan that does
not fit: <why>`.

## Rules the framework enforces

**At compile time**, each with its reason as the error (the cases are
`src/core/tests/compile_fail/settings_table_bad.cpp` and `type_spec_bad.cpp`): a bad row, a key
twice, a table not named for its folder, a spec or row in an unnamed namespace, a bank the spec
lacks or a misspelt one in `Io`, `ports()` or a context, a write into an input, a backwards
pass-through, a bad or doubled bank name, a `process` that may throw, a class that is final or has
no `ports` or `configure`, a member of the wrong signature (`configure`, `start`, `stop`, `check`,
`prepare`, `release`, the change version), a private or overloaded one, a static `start` (the old
interface), a call `CreateOf` does not offer, a sound card, RTP receiver, sink or source on the wrong
bank, a loop's name that is not 1 to 8 of `[a-z0-9-]` or names two loops, a state given to a loop
declared without one, a loop the spec lacks (in a context, a post or the harness), a post of a
function of another loop's state, a store whose rule has no `maxBytes`, more than 1 MiB or a mode
beyond `0777`, a store's suffix that is not 1 to 16 of `[a-z0-9-]` or names two stores, a store
the spec lacks (in a context or the harness), the spec's members out of order (`.stores` after
`.loops`), and the harness given a bank the spec lacks.

**At run time**, a rule the type breaks fails the node `start-failed` with the rule's text, even
when the type catches the throw; none takes ear down:

| Broken | The node's detail |
|---|---|
| `use` twice | `configure threw: use called twice` |
| `use` with a null code or an empty pass-through | `configure threw: use given no code: ...` |
| No `use` by an applied answer | `configure gave no code` |
| A pass-through the ports refuse | `configure gave a pass-through: <why>` |
| A sink with no consumer, or a consumer twice | `configure gave sink <bank> no consumer`; `configure threw: consumedBy called twice on sink <bank>` |
| Latency declared twice for one path (`declareLatency(from, to, ...)`), or twice for one bank (the legacy `declareLatency(bank, ...)`) | `configure threw: latency declared twice for the path <from> to <to>`; `configure threw: latency declared twice for bank <bank>` |
| Latency declared on a path the pass-through does not copy | `configure declared latency on <from> to <to>, a path its pass-through does not copy` |
| A latency plan that does not fit the ports (only a framework bug makes one: each declaration is checked at its call) | `configure gave a latency plan that does not fit: <why>` |
| A second companion on an inside bank | `configure threw: a second companion on bank <bank>` |
| A loop declared with a state given none, or two | `configure gave loop <name> no state`; `configure threw: loop <name> given a state twice` |
| Loops declared on a host with no loops adapter | `no loops in this host` (`configure threw: no loops in this host` at a loop call) |
| A loop that cannot start | `cannot start loop <name>: <why>` |
| A loop's handler throws | `loop <name>'s handler threw: <what>` (`start-failed`, or `lost` once rested; once the teardown began, a `log` record) |
| Stores declared on a host with no stores adapter | `no stores in this host` (`configure threw: no stores in this host` at `cx.store`) |
| A store read or written before its open completed (in `configure`) | `configure threw: store <file> was read before it was opened` (`... was written ...`) |
| A store's blocking `replace` on the main loop (a hook) | `<hook> threw: store <file>: replace waits, so never on the main loop: a hook uses replaceLater` |
| A store that cannot be opened, or whose text its `distrust` refuses | `<file>: <why>` ([stores](#stores)) |
| A `CreateOf` call after `configure` returned | `<call> outside configure` |
| `Configured::failed("")` | `configure answered failed with no detail` |
| `configure` throws | `configure threw: <what>` |
| `start()` throws | `start threw: <what>` (then `start-failed`, or `lost` once rested) |
| `stop()` throws during a stop | `stop threw: <what>`; during another teardown it is a `log` record |
| The code's `prepare` throws | `prepare threw: <what>` (`start-failed`, or `lost` once rested); that code is never run again, nor released |
| A sink's consumer throws | `the sink <bank>'s consumer threw in <begin\|consume>: <what>` (`start-failed`, or `lost` once rested); in `finish()`, during a restart's, disable's or removal's fade, a second fault during a stop, or once the teardown began, a `log` record |
| A legacy recipe, or a legacy handoff's consumer, throws | `recipe threw: <what>` (`start-failed`); `the handoff <bank>'s consumer threw in consume: <what>` (`start-failed`, or `lost` once rested; its `finish`, or once the teardown began, a `log` record); a run object's `close` that throws is a `log` record |
| A type's code throws at parse (a text row's parse function, `check`, `ports`, a legacy type's config) | the edit is refused, `invalid-settings` with `<type> threw: <what>`, as any invalid setting is |

**One catch.** Every call the framework makes into a type's code goes through one catch: the hooks,
the code's `prepare` and a legacy recipe and close on the main loop (each named in the main loop's
activity while it runs, so a stall inside one is placed), a sink's or legacy handoff's consumer on
that one's own thread, a loop's handlers on its thread, and the type's code run at parse. So nothing a type throws reaches PipeWire's
callbacks ([architecture.md](architecture.md#the-callback-boundary)); a rule broken while one runs
fails it first, even when the type caught the throw. A type's own `std::logic_error` is its bug like
any other throw. A bug of ear's own adapter inside a context call (a sink adapter's
`std::logic_error` inside `sink`) is not the type's: the node fails `lost`,
`internal error in <adapter call>: <what>`, said first in an `ear.internal-error` record, whether
or not the type caught it; an adapter's refusal (no thread, no eventfd) stays the node's
`start-failed` ([architecture.md](architecture.md#faults-by-owner)). A throw the node cannot take as its failure (a restart, disable or removal under
way, or a stop's fault already held) is an `ear` `log` record instead, so none goes unrecorded. A
code's `release` and `process`, and every destructor, are `noexcept`: a throw there is
`std::terminate`.

**Timing.** Each hook is timed; one that runs past 20 ms on the main loop is `ear.hook-slow` (once
per node and hook). Hooks never block: they acquire, construct and post.

## Testing a type

### Its own tests

Its tests include `<type>_spec.hpp` (`ear_node_type()` puts the folder on their include path) and
run the type through `ear::testing::TypeHarness<spec, Class>` (`ear/testing/type_harness.hpp`): a
real `NodeHost` over in-memory fakes, with no PipeWire. The test's thread stands in for the main
loop and the realtime thread both; each call returns with its transition applied as far as the
fakes allow. Banks are named by the spec's handles.

| Call | What |
|---|---|
| `TypeHarness h({.format = {48000, 256}, .node = "x"})` | Options: format, node name, `noRecordingsFolder` (a host with none: every recording refused), `autoComplete` (off: each fade-out and barrier waits for the test, or a fade-out for a cycle that ends it at zero, and a drive cancels a fade-out still waiting, as the real block does), `holdSinks` (each sink's consumer begins only at `h.ready(bank)`, not at configure's return), and `sinkRingFrames` (each sink's ring in frames instead of the size the type asked for, rounded up to a power of two as the engine's is: cycles that do not divide it make the sink split chunks at its end); for loops `holdLoops`, `refuseLoops` and `allowSourcesLeft`, and for stores and recordings `holdIo` (below) |
| `h.parse(settings)` | The type's parse: nothing, or the refusal's text |
| `h.add()`, `h.add({.disabled = true})` | Adds the node and configures it |
| `h.blockReady()` | The block becomes ready: the code drives, the node runs |
| `h.ready(bank)`, `h.lose(bank, detail)` | The companion on `bank` becomes ready (an engine companion told ready; a sink held by `holdSinks` begins its consumer; a sink that began or a source is ready already), or an engine companion is lost (a sink or a source is never lost by the framework: `std::logic_error`) |
| `h->advance(ms)` | The fake clock moves on: the framework's bounds and each companion's readiness deadline fire on it |
| `h.process(frames)` | One cycle, by the real block's own rules (`ear/core/block_rules.hpp`, `ear/core/cycle_facts.hpp`): its facts stated by `CycleAccount` ([below](#lost-cycles)), a discontinuity where they say (the first cycle, each drive's first, and where the calls below made one); each source played into its bank's input buffers first (by the same `SourceRing::play`, with its clock if it has one, its `SourceTick` carrying the playback latency its bank tells), outputs NaN-filled, the fade applied (in over 5 ms from each drive; a fade-out ramps down from the gain it has); each sink's chunk stamped with the capture latency its bank tells: what the real block works out by the same model, `workOut`, with no peer on any port ([engine.md](engine.md#latency-per-bank-pair)), and handed to its consumer at once through the same `SinkRing` |
| `h->loseCycles(n, Lost::cut)`, `h->loseCycles(n, Lost::notScheduled)`, `h->changeDriver(id)`, `h->reposition(frames)` | Before the next `process()`: `n` graph cycles start without the block (its position and time move on by that cycle's frames; past 2^32 - 1 before one `process()`, `std::logic_error`), each cut short or each run whole without it (paused); the block runs on another driver from then on; its position moves with no cycle missed and no time gone ([below](#lost-cycles)) |
| `h->facts()`, `h->cuts()` | The last cycle's `CycleFacts`, and the cut record the block's account reads |
| `h.feed(bank, interleaved)`, `h.lastPlayed(bank)`, `h.shortfall(bank)` | A source's writer, never waiting (it takes what fits and returns the frames taken); what the last `process()` did with its ring (the `SourcePlayed` its clock was told, all zero before the first); its shortfall |
| `h.input(bank, i)`, `h.output(bank, i)` | A port's buffer |
| `h.stop()`, `h.start()`, `h.restart()`, `h.change(settings)`, `h.disable()`, `h.enable()`, `h.remove()` | The operations |
| `h.phase()`, `h.health()`, `h.gone()`, `h.droppedFrames()`, `h.companionList()` | What the node shows; its companion list as a client will see it ([engine.md](engine.md#companions-in-a-nodes-list)), read now |
| `h->unwritten()`, `h->notes()`, `h->logs()`, `h->diagnostics()`, `h->latencyPlan()`, `h->ports()` | Through `->`, the underlying `NodeHarness`; `notes()` as the main loop heard them (after the [note rate](#the-node-handle)), `logs()` every log entry attributed to the node |
| `h.companion(bank)` | What was made on a bank (an engine companion's bank as `NodeHost` named it): its `kind` (`CompanionKind`: a card's by its bank's direction), `name`, `shape`, `made`, `ready`, `destroyed`, and a sink's `chunks` as its consumer got them, one per call (a chunk split at the ring's end is two) |
| `h.recordingRequests()`, `h.recordingBytes(path)`, `h.presetRecording(path, bytes)`, `h.failRecordingOpen(path, why)`, `h.pipeRecording(path)`, `h.openRecordings()`, `h->breakRecordings(what)` | The [recordings](#recordings): every one asked for; each an in-memory file by path (`./a.wav` is `a.wav`) with a real, seekable descriptor (a `memfd`), so libsndfile writes it as a file; what it holds now (nothing: never made), or before the node opens (and truncates) it; every open refused with `why` (`cannot open <path>: <why>`), or given a pipe's write end (made, but no WAV can be written to it); how many descriptors are open now; every open throwing, an adapter's bug. Each open and close is a job on the harness's I/O executor (`open recording <path>`, `close recording <path>`, `release recording <path>` for a dropped open's file, in `ioJobs()`), held by `holdIo` as a store's are |
| `h.runLoops()`, `h->advance(ms)`, `h.armed(loop)`, `h.readable(fd)`, `h.loopsReady()`, `h->loops()` | The loops ([loops](#loops)), on the test's thread with their kind kept: `runLoops()` runs what was posted and each watch rung (a source's clock wake rung by `process()`, a descriptor marked `readable(fd)`) until all are idle, never a timer; `advance` fires their timers in time order with the framework's (an `every` once per period passed: a handler takes no time); `armed(loop)` counts the sources armed on it whose handles live. Loops start when the framework asks (with `holdLoops`, at `loopsReady()`; with `refuseLoops = "<why>"`, never); a teardown turns each loop until it finished and closed; a source a state left armed fails the call that tore it down unless `allowSourcesLeft` (then only `ear.loop-source-left` in `diagnostics()`). `loops()` lists each loop made: its name, kind, and whether it closed to others, finished and closed |
| `h.presetStore(store, text, mode)`, `h.storeText(store)`, `h.storeMode(store)`, `h.failStoreOpen(store, why)`, `h.failNextWrite(store, why)`, `h.runIo()`, `h->ioJobs()`, `h->storeCalls()`, `h->breakStores(what)` | The [stores](#stores), by the spec's handle, on an in-memory disk under the framework's own store rules: a file put there before the node opens it, what it holds and its mode now, every open's read refused or the next write refused with `<file>: <why>`. Each open and write is a job on the harness's I/O executor, run as each call settles (with the option `holdIo`, only at `runIo()`); a blocking `replace` runs its job inside the call. `ioJobs()` names every job run (`open store <file>`, `write store <file>`, and `release store <file>` for a dropped open's handle), `storeCalls()` every disk call in order (`readNarrowed`, `read`, `narrow`, `replace`), `breakStores(what)` makes every disk call throw, an adapter's bug, and `overdueNextWrite(ran)` makes the next `replace`'s wait give up, after its job ran (`unconfirmed`, and it landed) or before it began (`refused`) |
| `h->internalErrors()` | Every adapter bug told about the node (the `ear.internal-error` record's callback, node, where and what), in order |

The harness does not see a class's `start()` and `stop()`; their pairing is tested in
`node_host_test.cpp`.

### Lost cycles

Each cycle the block runs states what happened since its last: `CycleFacts`
(`ear/core/cycle_facts.hpp`), worked out by one pure rule, `CycleAccount`, from the driver's tick
(its id, the graph cycle's number, the position, the frames and the raw start) and a `CutRecord`
of which graph cycles the driver cut short. A code reads them in `GraphCycle::facts`; the engine
numbers its cycles and records its cuts from the internal clock's own events
([engine.md](engine.md#graphcycle)).

| Fact | Meaning |
|---|---|
| `graphCycle` | The graph cycle it ran in, counted from 1; 0 off the clock that numbers them |
| `missed`, `missedCut` | Graph cycles started on the same driver since the block last ran one, and how many of them the driver cut short; `confirmed()` when every one was |
| `lastCut` | The cycle the block last ran was cut short after it, so nodes after it may not have heard it: `false` on the block's first cycle, none where nothing can answer (no record, the last cycle unnumbered, or the one undecided case below) |
| `jumpFrames`, `lostRawNs` | The position against where the last cycle ended (exact for any jump under 2^63 frames), and the raw time from that end to this start, on the same driver (0 otherwise) |
| `causes` | Why it is a discontinuity, a set: `start` (the block's first), `drive` (a new code's first), `driverChange`, `catchUp` (missed cycles, all cut, the position moved forward by exactly that many of this cycle's frames), `absent` (as `catchUp`, but some not cut: the block was not scheduled), `oversized` (after a cycle too large for the block), `reposition` (any other move: back, or not whole cycles, or cycles missed with no move). `whole()` is none, `discontinuity()` any |

Off the numbered clock the missed cycles come from the position alone (a forward jump of whole
cycles of this one's frames) and none is confirmed, so there a loss is always `absent` and a
reposition by whole cycles reads as that many missed. ear forces one quantum
([engine.md](engine.md#the-quantum-guard)), so a loss's cycles have this cycle's frames; were the
quantum to change across one, the rule fails toward `reposition`, never toward an excused
`catchUp`. The cuts are counted for any span, with no reach: a record keeps two running counts
(`cutsSoFar`, every cut once, and `newestWhole`, the newest cycle that ran whole), and the account
takes their difference since the last cycle's entry, so a stall of any length is `catchUp` with
every cycle counted. Only its ring of `cutRecordCycles` (2^16) slots, which answers which cycle
was cut (`cut(cycle)`), has a reach, and it is asked only when some missed cycle was not cut:
past its reach, with some but not all of the cuts since the last cycle, whether that cycle was one
of them stays undecided (`lastCut` none, `missedCut` one fewer than it may be). The facts are the
one home of what was lost: a source's clock reads them in its tick and a sink's stamp carries them from `GraphCycle`
([engine.md](engine.md#graphcycle)), and no consumer works a loss out from `position`.

The harness's fake block runs `CycleAccount` over its own `HarnessCuts`
(`ear/testing/harness_cuts.hpp`), the engine's own record (`CutRing`), told as the engine's driver
tells: a cycle starts, then may be marked cut. It keeps `CutRecord`'s contract for readers on other threads (the header says how), so
a test may read it while cycles run. `loseCycles`, `changeDriver` and `reposition` make the facts
a type must survive (`catchUp` and a pure `reposition` on the harness's own, numbered clock only;
after `changeDriver`, as above), and
`facts()` is what the last `process()` stated (its code's `GraphCycle::facts`), so a type's tests
can lose a cycle and check what its code does with the facts the engine would hand it. The rows every runner of the rule
must agree on are `test/support/cycle_table.hpp`'s ([testing.md](testing.md#what-each-test-program-proves)).

The facts also feed ear's diagnostics counters ([engine.md](engine.md#counters)), which a type
never writes: the framework counts them. In the harness `counts()` is the node's, as `NodeHost`
gives them, counted by the fake block as the real block counts: `loseCycles(n, Lost::notScheduled)`
moves `missedCycles` by n, `reposition` and `changeDriver` move `repositions` and `driverChanges`,
and a code whose `process()` takes longer than its cycle (timed on the real clock) moves
`overruns`. `Lost::cut` moves no node's counter, only the graph's: `graphCounts()` counts the
harness's own record, its cycles, cut cycles and stalls.

### What every type must also pass

- **Valid settings.** Add an entry to `validSettings` in `test/nodes/all_types.hpp`: one or more
  settings objects that together give every key the type declares. The all-type tests (which list
  no type: they take them from `allNodeTypes()`) fail naming the type without it. They run every
  type through the harness: added, configured, running once every engine companion is ready, every
  output sample written, then stopped, started, restarted, disabled, enabled and removed. (A type
  that cannot run there yet is listed in `notYetInHarness`; today only `sendspin`.)
- **Refusals.** Add a test case to `test/nodes/settings_refusals_test.cpp` giving each refusal's
  exact text and which of several problems is told first.

### Removing a type

Delete its folder and its entry in `validSettings` (and `notYetInHarness`, if there), its cases in
`settings_refusals_test.cpp` (a case left there fails naming the type) and its name in
`test/nodes/node_types_test.cpp`.

## The old interface

`sendspin` alone is still written on the old type interface: a `NodeTypeOf<table>` whose config's
`start(StartContext&)` returns a `StartResult` with a recipe that builds a `RunObject` with its
threads, its inputs going through a legacy handoff. It runs under the same lifecycle through the
framework's legacy adapter (`src/core/legacy_instance.hpp`), with no `stop()` hook, so a stopped
`sendspin` node keeps its connection. Do not copy it; a new type uses the class above.

## Worked example: `tone`

A sine generator: `frequency` (Hz, default 440) and `channels` (default 1), the same tone on every
channel at half scale. It refuses, at configure, a frequency at or above half the graph frequency,
which parse cannot judge because it does not know the graph frequency.

The tests compile this example as it stands on this page (`ear_doc_example_type` and
`ear_doc_example_tests`, after `ear_doc_example_extract` takes each file from a level-3 heading
naming its path under `src/nodes/tone/` and the one fenced block under it), so it cannot drift from
the headers. Keep that shape when you edit it.

### `src/nodes/tone/CMakeLists.txt`

```cmake
# ear_node_tone: the "tone" node type, a sine on every output channel.
ear_node_type(SOURCES tone.cpp TESTS tests/tone_test.cpp)
```

### `src/nodes/tone/include/ear/nodes/tone.hpp`

```cpp
#pragma once
#include <memory>
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "tone": outputs "out.1".."out.N", a sine at half scale; settings {"frequency"?, "channels"?}.
std::unique_ptr<const NodeType> makeToneNodeType();
} // namespace ear::nodes
```

### `src/nodes/tone/tone_spec.hpp`

```cpp
#pragma once
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>

#include "ear/core/lifecycle.hpp"

namespace ear::nodes::tone {

inline constexpr auto frequencyRow = setting::defaulted("frequency", setting::number(1, 20000, "Hz"), 440);
inline constexpr auto channelsRow = setting::defaulted("channels", setting::number(1, 8, "channels"), 1);
inline constexpr auto out = bank::output<"out">;
inline constexpr TypeSpec spec{.settings = SettingsTable<frequencyRow, channelsRow>{"tone"}, .banks = Banks<out>{}};
using Checked = CheckedOf<spec>;

/// Runs on the realtime thread: reads no clock, allocates nothing, writes every output sample.
struct ToneCode {
	double step = 0; // radians a frame
	double phase = 0;

	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (std::uint32_t frame = 0; frame < io.frames(); ++frame) {
			const float sample = 0.5F * static_cast<float>(std::sin(phase));
			for (const std::span<float> channel : io[out]) channel[frame] = sample;
			phase += step;
			if (phase >= 2 * std::numbers::pi) phase -= 2 * std::numbers::pi;
		}
	}
};

struct Tone {
	static Members<spec> ports(const Checked& s) { return {out(members(s.get<channelsRow>()))}; }

	Configured configure(CreateOf<spec>& cx, const Checked& s) {
		const double rate = cx.format().rate.hz(); // its number, for the sine's step (engine.md#dating)
		const double frequency = s.get<frequencyRow>();
		if (frequency * 2 >= rate)
			return Configured::failed("frequency " + std::to_string(s.get<frequencyRow>()) +
			                          " Hz is not below half the graph frequency");
		cx.use(ToneCode{.step = 2 * std::numbers::pi * frequency / rate});
		return Configured::applied();
	}
};

} // namespace ear::nodes::tone
```

### `src/nodes/tone/tone.cpp`

```cpp
#include <memory>

#include "ear/nodes/tone.hpp"
#include "tone_spec.hpp"

namespace ear::nodes {

std::unique_ptr<const NodeType> makeToneNodeType() { return makeNodeType<tone::spec, tone::Tone>(); }

} // namespace ear::nodes
```

### `src/nodes/tone/tests/tone_test.cpp`

```cpp
#include <cmath>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/nodes/tone.hpp"
#include "ear/testing/type_harness.hpp"
#include "tone_spec.hpp"

namespace {
namespace tone = ear::nodes::tone;
using Harness = ear::testing::TypeHarness<tone::spec, tone::Tone>;
} // namespace

TEST_CASE("Tone declares out.1 to out.N and no inputs", "[tone]") {
	Harness harness;
	REQUIRE_FALSE(harness.parse({{"channels", 3}}));
	CHECK(harness->ports().inputs().empty());
	CHECK(harness->ports().outputs() == std::vector<ear::PortName>{"out.1", "out.2", "out.3"});
}

TEST_CASE("Tone runs once its block is ready and writes a sine on every channel", "[tone]") {
	Harness harness({.format = {.rate = ear::SampleRate::fixed<48000>(), .maxBlock = 256}, .node = "beep"});
	REQUIRE_FALSE(harness.parse({{"frequency", 1000}, {"channels", 2}}));
	harness.add();
	CHECK(harness.phase() == "starting"); // nothing to acquire: configured, waiting for its block
	harness.blockReady();
	CHECK(harness.phase() == "running");
	harness.process();
	harness.process(); // past the 5 ms fade-in
	CHECK(harness->unwritten() == 0);
	const auto left = harness.output(tone::out, 0);
	const auto right = harness.output(tone::out, 1);
	for (std::size_t i = 0; i < left.size(); ++i) {
		CHECK(left[i] == right[i]);
		CHECK(std::abs(left[i]) <= 0.5F);
	}
}

TEST_CASE("Tone fails a frequency at or above half the graph frequency", "[tone]") {
	Harness harness({.format = {.rate = ear::SampleRate::fixed<16000>(), .maxBlock = 256}});
	REQUIRE_FALSE(harness.parse({{"frequency", 8000}}));
	harness.add();
	CHECK(harness.phase() == "failed");
	CHECK(harness.health().reason->detail == "frequency 8000 Hz is not below half the graph frequency");
}

TEST_CASE("Tone stops silent and starts again", "[tone]") {
	Harness harness;
	REQUIRE_FALSE(harness.parse(ear::NodeSettings::object())); // {} would be JSON null, which tone refuses
	harness.add();
	harness.blockReady();
	harness.stop();
	CHECK(harness.phase() == "stopped");
	harness.start();
	CHECK(harness.phase() == "running");
}
```

### Outside the folder

In `test/nodes/all_types.hpp`'s `validSettings`:

```cpp
{"tone", {{{"frequency", 440}, {"channels", 2}}}},
```

In `test/nodes/settings_refusals_test.cpp`:

```cpp
TEST_CASE("Tone's refusals, in order", "[nodes][settings]") {
	CHECK(says("tone", "3") == R"(settings: must be an object with optional "frequency" and "channels")");
	CHECK(says("tone", R"({"gain":1})") == "unknown key gain");
	CHECK(says("tone", R"({"frequency":0})") == "frequency: must be a whole number of Hz, 1-20000");
	CHECK(says("tone", R"({"channels":9})") == "channels: must be a whole number of channels, 1-8");
	CHECK(says("tone", "{}") == accepted);
}
```

Configure now prints `Node types: card, file, rtp, sendspin, tone, white`, and `ear.conf` takes:

```conf
node tone beep { frequency 1000; channels 2; }
route beep:out.1 desk:play.1;
```
