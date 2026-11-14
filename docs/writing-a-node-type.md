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
- `ear_node_type()` (`cmake/NodeType.cmake`) makes the library (with a header set checked on its
  own, linking `ear_core`) and, under `BUILD_TESTS`, the test program `ear_node_<type>_tests`
  linking the type harness. A folder without the call, without `SOURCES` and `TESTS`, without its
  public header, or a stray file beside the folders in `src/nodes/`, stops configure.
- Plain CMake after the call adds any outside library the type needs, `PRIVATE`, so no other
  target sees it (`target_link_libraries(ear_node_file PRIVATE PkgConfig::SNDFILE)`).
- **No type links another, nor `ear_runtime` or `ear_pipewire`**: configure refuses it once the
  whole project is read, through a helper library, an alias or a generator expression too. A type
  reaches the engine and the framework's threads only through `ear_core`'s interfaces; its test
  program may link what it needs.
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
| `explicit Type(NodeOf<spec>&)` or `Type()` | one of them | `NodeOf` is valid for the instance's life: `format()`, `node()`, and `handle()` |
| `Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>& s)` | yes | Acquire and construct only, on the main loop, then answer |
| `void start()` | no | Called once its code is driven (the node is then `running`) |
| `void stop()` | no | Called once before each silence that follows a `start()` that returned |
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
| `cx.openRecording(RecordingRequest{path})` | A file created or truncated beneath the recordings folder; its descriptor, or why not. The framework closes it after the instance | Answer `Configured::failed("cannot open " + path + ": " + error)` on a refusal |
| `cx.sink(bank, SinkShape{.frames = n}).consumedBy(std::make_unique<Consumer>(...))` | A sink on an inside output bank ([sinks](#sinks)) | Inside output banks only; exactly one consumer; one companion per bank |
| `SourceWriter& w = cx.source(bank, SourceShape{.frames = n}, clock)` | A source on an inside input bank ([sources](#sources)): what the type's own thread writes, played into the bank before the code; `clock`, a `std::unique_ptr<SourceClock>`, its own clock ([a source's clock](#a-sources-clock)), left out for a source with none | Inside input banks only (compile time); `0 < n <= maxSourceFrames` (2^24); one companion per bank |
| `cx.declareLatency(from, to, Latency{min, max})` | The code's own latency on the path from an input bank to an output bank, in frames: carried as a path through the block, so it counts in both banks' told latency ([latency](#latency)) | Once per path (a mixer declares each of its inputs' paths into one output); with a pass-through, one of its pairs; a plan that does not fit the ports fails the node `start-failed` |

Then answer `Configured::applied()`, or `Configured::failed(detail)` for what only the type can see
(the node fails `start-failed: <detail>`). A `CreateOf` is valid only inside the `configure` it is
handed to. What a type needs from outside it acquires here; a type's factory takes no arguments.

Each call is served by the host's adapter of its kind (`ResourceAdapters`, one member per kind,
`node_host.hpp`). ear's host has every kind; a host without one (a test's) refuses the call as an
adapter's refusal is: the node fails `start-failed: configure threw: no <kind> in this host`
(`sinks`, `sources`, `sound cards`, `RTP receivers`), and `openRecording` answers
`no recordings folder`.

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
- A sink's bank is never faded, so a recording keeps exact samples.

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
bank, and the harness given a bank the spec lacks.

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
that one's own thread, and the type's code run at parse. So nothing a type throws reaches PipeWire's
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
| `TypeHarness h({.format = {48000, 256}, .node = "x", .recordings = &adapter})` | Options: format, node name, a recordings adapter (none: every recording refused), `autoComplete` (off: each fade-out and barrier waits for the test, or a fade-out for a cycle that ends it at zero, and a drive cancels a fade-out still waiting, as the real block does), `holdSinks` (each sink's consumer begins only at `h.ready(bank)`, not at configure's return), and `sinkRingFrames` (each sink's ring in frames instead of the size the type asked for, rounded up to a power of two as the engine's is: cycles that do not divide it make the sink split chunks at its end) |
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
| `h.recordingRequests()` | Every recording asked for |

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
