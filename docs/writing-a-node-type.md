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
- **No type links another**: configure refuses it once the whole project is read, through a helper
  library, an alias or a generator expression too.
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
inline constexpr auto rateRow = setting::optional("rate", setting::number(8000, 192000, "Hz"));
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
| `setting::text<Parse>(mustBe)` | a JSON string `Parse` accepts; `Parse` is `std::optional<T>(std::string_view)`, and `T` (what the row reads) must own what it holds | `<key>: must be <mustBe>` |

- `.onlyWithout(above)` refuses the key given together with a row above it: `<key>: only without
  "<above>"`. Neither row may be required.
- Each key once; `disabled` (the framework's key) cannot be declared; a table's name must be its
  folder's. Each is a compile error.
- The table is `SettingsTable<row1, row2, ...>{"<type>"}` (`SettingsTable<>{"white"}` for none).
  It gives the type's name, its settings' kinds (text or whole number, which is how `ear.conf` is
  read and written) and the whole check, so the graph file reader cannot disagree with `parse`.
- A parsed value is parsed once: a text row reads as what its parse function made (a `CardId`, an
  `RtpAddress`, a path).

### Banks: the ports

```cpp
inline constexpr auto play = bank::input<"play">;             // routable: routes name play.1, play.2, ...
inline constexpr auto capture = bank::output<"capture">;
inline constexpr auto toCard = bank::insideOutput<"to_card">;   // inside: only the node's own outside nodes and code
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
| `cx.format()`, `cx.node()` | The graph's rate and block size; the node's name | |
| `cx.use(code)` | The code: a struct with `process`, or `passThrough({bankPair(in, out), ...})` | Exactly once |
| `cx.soundCard(bank, SoundCardRequest{.card, .direction})` | A sound card joined to an inside bank: playback on an inside output, capture on an inside input. Named `<node>.playback` or `<node>.capture`; made once the block is ready | Inside banks only (compile time); the direction must agree with the bank's |
| `cx.rtpReceiver(bank, RtpReceiverRequest{...})` | An RTP receiver joined to an inside input bank, `<node>.rtp` | Inside input banks only |
| `cx.openRecording(RecordingRequest{path})` | A file created or truncated beneath the recordings folder; its descriptor, or why not. The framework closes it after the instance | Answer `Configured::failed("cannot open " + path + ": " + error)` on a refusal |
| `cx.sink(bank, SinkShape{.frames = n}).consumedBy(std::make_unique<Consumer>(...))` | An interim sink on an inside output bank ([sinks](#sinks)) | Inside output banks only; exactly one consumer |
| `cx.declareLatency(from, to, Latency{min, max})` | The code's own latency, from an input bank to an output bank, in frames | At most once per output bank |

Then answer `Configured::applied()`, or `Configured::failed(detail)` for what only the type can see
(the node fails `start-failed: <detail>`). A `CreateOf` is valid only inside the `configure` it is
handed to. What a type needs from outside it acquires here; a type's factory takes no arguments.

### Answers, notes and failures from the type's own threads

`NodeOf::handle()` gives the configuration's `InsidesEvents`, which a thread or a sink consumer the
instance owns may keep and call from any thread until the configuration's teardown ended:

- `say(text)`: a note, `node <n> says <text>` on standard output. Never a state.
- `lost(detail)`: the node fails, `start-failed: <detail>` before it first rested `running` or
  `stopped`, `lost: <detail>` after.

What a configuration's threads tell lands only on that configuration, never on the next one.

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
  (`startRawNs`), the driver's `position`, `rateDiff` and `driverId`, `nextNs`, `xruns`,
  `discontinuity`, and `downstreamOf(output)`, the latency downstream of an output port. Never read
  a clock yourself ([engine.md](engine.md#graphcycle)).
- A code written against another type's spec compiles but fails the node at `use`.
- **The pass-through.** `passThrough({bankPair(fromBank, toBank), ...})` copies each input bank's
  ports to an output bank's, member for member, and silences every output no pair names. Banks of
  a pair must be the same size, an output bank written once. `card` and `rtp` write no audio code
  of their own: they pass between their inside and public banks.

## Sinks

A sink takes audio off the realtime thread. The type asks for one on an inside output bank:

```cpp
cx.sink(writer, SinkShape{.frames = 2 * cx.format().rate})
	.consumedBy(std::make_unique<WavWriter>(...));
cx.use(passThrough({bankPair(record, writer)}));
```

- Each cycle the code ran, after the code, the block interleaves the bank's frames into the sink's
  lock-free ring with a stamp (the cycle's position, both clocks, a discontinuity flag) beside them,
  and wakes the sink's own thread.
- The `SinkConsumer` is called on the sink's own thread only, one call at a time: `begin()` once,
  before the node is `configured` (open what it writes to; a failure is `handle().lost(detail)`,
  which fails the node `start-failed`); `consume(const Chunk&)` for each chunk in order;
  `finish()` once at the teardown, after the last chunk. None may throw. Each may block on I/O.
- A sink takes exactly the cycles its code drove: a stop's silence ends them, a start begins them
  again; a stop closes nothing. A gap shows only in `Chunk::position`; `discontinuity` is set on the
  first chunk after any drive.
- The frame ring and the stamp ring drop together (no frame without its stamp); dropped frames are
  counted and reported as the node's `dropped=` at removal.
- A sink's bank is never faded, so a recording keeps exact samples.

## Latency

A type declares only **its own** latency: how long its code holds audio between an input bank and
an output bank (`rtp`'s receive buffer, `sendspin`'s feed). The framework carries everything else:
a pass-through's pair forwards what lies beyond it, each block tells its linked ports, and the code
reads the latency downstream of its outputs in `GraphCycle`
([engine.md](engine.md#latency-per-bank-pair)).

## Rules the framework enforces

**At compile time**, each with its reason as the error (the cases are
`src/core/tests/compile_fail/settings_table_bad.cpp` and `type_spec_bad.cpp`): a bad row, a key
twice, a table not named for its folder, a spec or row in an unnamed namespace, a bank the spec
lacks or a misspelt one in `Io`, `ports()` or a context, a write into an input, a backwards
pass-through, a bad or doubled bank name, a `process` that may throw, a class that is final or has
no `ports` or `configure`, a member of the wrong signature (`configure`, `start`, `stop`, `check`,
`prepare`, `release`, the change version), a private or overloaded one, a static `start` (the old
interface), a call `CreateOf` does not offer, a sound card, RTP receiver or sink on the wrong bank,
and the harness given a bank the spec lacks.

**At run time**, a rule the type breaks fails the node `start-failed` with the rule's text, even
when the type catches the throw; none takes ear down:

| Broken | The node's detail |
|---|---|
| `use` twice | `configure threw: use called twice` |
| `use` with a null code or an empty pass-through | `configure threw: use given no code: ...` |
| No `use` by an applied answer | `configure gave no code` |
| A pass-through the ports refuse | `configure gave a pass-through: <why>` |
| A sink with no consumer, or a consumer twice | `configure gave sink <bank> no consumer`; `configure threw: consumedBy called twice on sink <bank>` |
| Latency twice for a bank | `configure threw: latency declared twice for bank <bank>` |
| A card whose direction disagrees with its bank | `configure threw: ... joins an inside output bank, not "<bank>"` |
| A `CreateOf` call after `configure` returned | `<call> outside configure` |
| `Configured::failed("")` | `configure answered failed with no detail` |
| `configure` throws | `configure threw: <what>` |
| `start()` throws | `start threw: <what>` (then `start-failed`, or `lost` once rested) |
| `stop()` throws during a stop | `stop threw: <what>`; during another teardown it is a `log` record |

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
| `TypeHarness h({.format = {48000, 256}, .node = "x", .recordings = &adapter})` | Options: format, node name, a recordings adapter (none: every recording refused) |
| `h.parse(settings)` | The type's parse: nothing, or the refusal's text |
| `h.add()`, `h.add({.disabled = true})` | Adds the node and configures it |
| `h.blockReady()` | The block becomes ready: the code drives, the node runs |
| `h.ready(bank)`, `h.lose(bank, detail)` | An outside node on `bank` becomes ready, or is lost |
| `h.process(frames)` | One cycle: outputs NaN-filled first, the 5 ms fade applied |
| `h.input(bank, i)`, `h.output(bank, i)` | A port's buffer |
| `h.stop()`, `h.start()`, `h.restart()`, `h.change(settings)`, `h.disable()`, `h.enable()`, `h.remove()` | The operations |
| `h.phase()`, `h.health()`, `h.gone()`, `h.droppedFrames()` | What the node shows |
| `h->unwritten()`, `h->notes()`, `h->diagnostics()`, `h->latencyPlan()`, `h->ports()` | Through `->`, the underlying `NodeHarness` |
| `h.companion(bank)` | What was made on a bank: a card's or receiver's request, `made`, `ready`, `destroyed`, and a sink's `chunks` as its consumer got them |
| `h.recordingRequests()` | Every recording asked for |

The harness does not see a class's `start()` and `stop()`; their pairing is tested in
`node_host_test.cpp`.

### What every type must also pass

- **Valid settings.** Add an entry to `validSettings` in `test/nodes/all_types.hpp`: one or more
  settings objects that together give every key the type declares. The all-type tests (which list
  no type: they take them from `allNodeTypes()`) fail naming the type without it. They run every
  type through the harness: added, configured, running once every outside node is ready, every
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
inline constexpr TypeSpec spec{.settings = SettingsTable<frequencyRow, channelsRow>{"tone"},
                               .banks = Banks<out>{}};
using Checked = CheckedOf<spec>;

/// Runs on the realtime thread: reads no clock, allocates nothing, writes every output sample.
struct ToneCode {
	double step = 0;  // radians a frame
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
		const double rate = cx.format().rate;
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
	Harness harness({.format = {.rate = 48000, .maxBlock = 256}, .node = "beep"});
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
	Harness harness({.format = {.rate = 16000, .maxBlock = 256}});
	REQUIRE_FALSE(harness.parse({{"frequency", 8000}}));
	harness.add();
	CHECK(harness.phase() == "failed");
	CHECK(harness.health().reason->detail == "frequency 8000 Hz is not below half the graph frequency");
}

TEST_CASE("Tone stops silent and starts again", "[tone]") {
	Harness harness;
	REQUIRE_FALSE(harness.parse({}));
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
