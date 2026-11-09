# Root classes: the architecture behind slice 3's fix clusters

Status: R1 built (`ear/core/latency.hpp`, section 3.5 says what changed building it), R2 built (the block and the
harness on `workOut`, `docs/engine.md#latency-per-bank-pair`; section 3.6), R4a built (section 4.5 says what
changed from 4.3), R5a built (its tools and guard: `docs/testing.md#the-tools`, section 5.4), R5b built
(`ear_pipewire_tests` audited, section 5.5) and R6 built (the property table: `docs/engine.md#properties-ear-sets`,
section 6.5 says what changed from 6.3); the rest designed, not built. Its steps are taken between slice 3b.4 and
slice 3c
([README](README.md)). The built system it changes is described in `docs/engine.md`, `docs/writing-a-node-type.md`,
`docs/testing.md` and `docs/output-records.md`; this page cites them and says only what changes.

## 1. The owner's words that bind this page

- (2026-11-09) "Reminder: always patch the root class of a bug, not the bug itself. Clusters of issues are a smell that
  signals an architecture problem."

## 2. The classes

Independent reviews of slices 3a and 3b found bugs that were each fixed where they were found. They fall into five
classes. Each section below states the class, the evidence, the fix that makes the class impossible or caught by
construction, its interfaces and its migration; section 8 orders the steps.

| # | Class | Fix | Steps |
|---|---|---|---|
| 1 | Latency rules written in several places, PipeWire's combine rule implicit, series and parallel in one type | One pure latency model in `ear_core`, used by the block and the harness; dating derived from it | R1, R2, R3 |
| 2 | Exceptions reach PipeWire's C callbacks, caught (or not) one call site at a time | One callback boundary every callback is registered through; a guard refuses a raw registration | R4a, R4b, R4c |
| 3 | Tests that depend on what the host happens to do | Inducers, a settled-engine wait, a clean-cycle ledger; the suite audited; a guard on sleeps | R5a to R5d |
| 4 | PipeWire properties whose couplings were learned one failure at a time | One property table in code, checked at compile time, against the live engine and against `docs/engine.md` | R6 |
| 5 | The harness re-implements engine rules, so a type's tests cannot see the bug | Each modelled rule is one pure function both call; a conformance test runs the same rows through both | R2, R7 |

## 3. Class 1: latency

### 3.1 The root class

A latency fact is computed by hand wherever it is needed, in two units (PipeWire's `spa_latency_info` triples and
ear's `Latency` frames), with one type for both a path and a combination of paths, so nothing says whether two values
are in series or in parallel, and the dating that uses them is a formula repeated per consumer. Evidence:

| Bug | Fixed in | What went wrong |
|---|---|---|
| A wrapped sink chunk's second call dated early by the first's frames | e4b72cf661 (3b.1) | `handoffs.cpp:225-250` advances position but the start times were not; frames-to-ns conversion is a private helper there |
| A source's `Played` dated early: the code's declared latency missing from its input bank's told latency | 6c1299b556 (3b.2) | `port_blocks.cpp:397-408` and `node_harness.cpp:521-548` each fixed separately |
| rtp told `{0,B}` instead of `{B,B}`: a companion's buffer combined as a parallel peer, not in series | 40be60af04 (3b.3) | `port_blocks.cpp:337-355` and `node_harness.cpp:495-519` each fixed separately |
| `quantum-limit` at the block size capped the ALSA card's buffer at four blocks | 3b.4 (in review) | A property's effect on latency, found by its test (class 4) |
| A latency test read a probe before the retell reached it | 40be60af04 | Waited for one hop's sighting (class 3) |

Every latency fix edited both `port_blocks.cpp` and `node_harness.cpp` (e4b72cf661, 6c1299b556, 40be60af04): the rule
has two homes. Today it is spread over `FilterPortBlock::combined()`, `frames()` and `retell()`
(`port_blocks.cpp:332-430`), the harness's `toldUpstream`/`toldDownstream` (`node_harness.cpp:495-548`), the `beyond`
list NodeHost builds (`node_host.cpp:1303-1319`), and four dating formulas: `ThreadSink::consume`
(`handoffs.cpp:228-250`), `SourceRing`'s `heardRawNs` (`source_ring.cpp:235-236`), `SendspinCode::process`
(`sendspin_code.cpp:39-42`) and `ToServer::capturedAtUs` (`to_server.cpp:34-37`). The plan's storage is asymmetric: a
pair's latency is stored under its output bank (`node_host.cpp:498-506`) and read back as "through" on the input side,
the shape of the 3b.2 bug.

Found writing this page, the same class, not yet fixed: `sendspin` dates the audio it sends upstream at the cycle's
start (`sendspin_code.cpp:25`, `capture_.publish({framesWritten_, startRawNs})`), ignoring the capture latency upstream
of its `to` bank, so a card's capture reaches the server dated late by the card's buffer. `GraphCycle` carries
`downstream` but no `upstream`; a sink's `Chunk` has `upstreamFrames`, and `sendspin` moves onto a sink in 3c.

### 3.2 The fix

One pure model, `ear/core/latency.hpp`, is the only code that combines latency or turns frames into time:

- **Series and parallel are different operations on different types.** `Latency` adds only in series (`+`). Parallel
  paths are a `Paths` accumulator, which alone takes the least of the leasts and the most of the mosts (PipeWire's
  `spa_latency_info_combine` rule, encoded once) and distinguishes "no path" from zero (a port with no peer is no path,
  `docs/engine.md#latency-per-bank-pair`). A beyond entry added as a peer is then a type error, not a review finding.
- **One unit.** The model works in frames. The graph has one quantum and one rate, fixed at boot, so a peer's
  `{quantum, rate, ns}` converts exactly at the port, once, in `ear_pipewire`'s `latency_spa.hpp`, before it is
  combined; what the block tells its peers is converted back in rate units only. PipeWire's per-unit combine gives an
  upper bound when a bank's peers mix units; frames first gives the exact least and most. Nothing outside ear reads
  what the block tells (no session manager, no socket), so only `docs/engine.md#sinks`' sentence on "field by field"
  changes.
- **The plan names roles, not storage.** `LatencyPlan` keeps `pairs` and `beyond`, and its `internal` list becomes
  `onPair` (a code's delay on a pair's path, keyed by the pair) and `onBank` (a declaration on one bank). The model is
  its only reader.
- **The rules are one function.** `workOut` takes the bank layout, the plan and each port's peer latency and returns
  every bank's beyond, its told latency, and each output port's downstream. The block calls it from `retell()` with the
  peers it heard; the harness calls it with no peers. Neither combines anything itself.
- **Dating is derived.** `FrameClock` turns frames into ns at the graph's rate, and `heardRawNs` and `enteredRawNs` are
  the two dating rules (`docs/engine.md#sinks`, `#sources`). `ThreadSink`'s split, `SourceRing`, `SendspinCode` and
  (in 3c, as a sink's consumer) `ToServer` call them; no other file divides by `1'000'000'000`.

Tested by table in `ear_core_tests` (`latency_test.cpp`): the algebra (series is associative, `Paths` of one is that
one, an empty `Paths` is no path, a no-path peer leaves a bank's combination unchanged), then one row per bank shape:
an input and an output bank with and without peers, a pair with a declared delay, a bank with a `beyond` entry and
peers, a source's `[0, capacity]`, a receiver's `{B, B}` behind zero-latency peers, a card block's two directions kept
apart, and each regression above as its own row. `ear_pipewire_tests` adds the conversion rows, including one per
unit checked against `spa_latency_info_combine` itself, so a PipeWire bump that changes the rule fails there.

### 3.3 Interfaces

```cpp
// src/core/include/ear/core/latency.hpp: pure; node_type.hpp includes it for Latency.
namespace ear {
/// A latency in frames at the graph frequency: the least and the most over the paths it stands for. Adds only in
/// series (one path after another).
struct Latency {
	std::uint32_t minFrames = 0;
	std::uint32_t maxFrames = 0;
	friend bool operator==(const Latency&, const Latency&) = default;
	friend constexpr Latency operator+(Latency a, Latency b) noexcept; // series: least + least, most + most
};
/// Parallel paths: the only way to combine latencies side by side (PipeWire's rule: the least of the leasts, the most
/// of the mosts). Empty is no path, which is not zero.
class Paths {
public:
	constexpr void add(Latency path) noexcept;
	constexpr void add(std::optional<Latency> path) noexcept; // none: no path, left out
	constexpr std::optional<Latency> combined() const noexcept;
	constexpr Latency orZero() const noexcept; // a bank with no path tells zero
};
/// What a configuration declares, by role (docs/engine.md#latency-per-bank-pair).
struct LatencyPlan {
	std::vector<BankPair> pairs;
	std::vector<std::pair<BankPair, Latency>> onPair;      // the code's delay on a pair's path
	std::vector<std::pair<std::string, Latency>> onBank;   // declared on one bank
	std::vector<std::pair<std::string, Latency>> beyond{}; // a companion's, in series beyond an inside bank
};
/// What the block tells and publishes, worked out afresh from the plan and what the peers report.
struct LatencyFacts {
	std::vector<Latency> told;       // per port, inputs then insideInputs, outputs then insideOutputs (playback/capture)
	std::vector<Latency> downstream; // per output port: what lies beyond its bank (GraphCycle::downstream)
};
/// The rules, once. `inputPeers`/`outputPeers` hold one entry per port in the same order; none where no peer reported.
LatencyFacts workOut(const Ports& ports, const LatencyPlan& plan, std::span<const std::optional<Latency>> inputPeers,
                     std::span<const std::optional<Latency>> outputPeers);
/// Frames as time at the graph's rate, and the two dating rules.
class FrameClock {
public:
	explicit constexpr FrameClock(std::uint32_t rate) noexcept;
	constexpr std::int64_t ns(std::int64_t frames) const noexcept;
	/// When frame `offset` of a cycle that started at `startRawNs` is heard, `downstream` beyond it (its most).
	constexpr std::int64_t heardRawNs(std::int64_t startRawNs, std::uint32_t offset, Latency downstream) const noexcept;
	/// When frame `offset` of that cycle entered the graph, `upstream` before it (its most).
	constexpr std::int64_t enteredRawNs(std::int64_t startRawNs, std::uint32_t offset, Latency upstream) const noexcept;
};
} // namespace ear

// src/pipewire/latency_spa.hpp: private to ear_pipewire, the one place the units meet.
namespace ear::pipewire {
std::optional<Latency> fromSpa(const spa_latency_info& info, std::uint32_t quantum, std::uint32_t rate) noexcept;
spa_latency_info toSpa(Latency latency, spa_direction direction) noexcept; // rate units only
} // namespace ear::pipewire
```

### 3.4 Migration

- **R1** (pure addition): `latency.hpp`, `latency.cpp`, `latency_test.cpp`; `Latency` moves there from
  `node_type.hpp`, which includes it (no caller changes).
- **R2** (after 3b.4 lands): `port_blocks.cpp`'s `combined()`, `frames()` and `retell()` become a call to `workOut`
  plus `latency_spa.hpp` at the port; the peers are kept as `std::optional<Latency>` per port (the `*Heard_` flags go).
  `node_harness.cpp`'s `toldUpstream`/`toldDownstream` become `workOut` with no peers. NodeHost fills `onPair`/`onBank`
  (`node_host.cpp:498-511`, `1303-1319`); `LatencyPlan` moves from `node_host.hpp` to `latency.hpp`. The conformance
  test (section 7) runs the table's rows through the real block. The library docs' latency sections say the model is
  the rule's home.
- **R3**: `handoffs.cpp`'s `framesNs`, `source_ring.cpp:235`, `sendspin_code.cpp:39-42` call `FrameClock`; `ToServer`
  takes its upstream with 3c's sink (3c's design gains the line).

### 3.5 Found building R1

What R1 built differs from 3.3 where 3.3 could not work as declared; the library page that describes it is
`docs/engine.md#latency-per-bank-pair`.

- **`Delay`.** With `beyond` holding a `Latency`, a beyond entry added as a peer compiles, so 3.2's type error could
  not hold. What lies in series on every path is now `Delay` (made only explicitly from a `Latency`, converting to
  nothing; `Latency + Delay` is a `Latency`), and `Paths::add` refuses one with a `static_assert`. `onPair`, `onBank`
  and `beyond` hold `Delay`. The compile-fail cases are `src/core/tests/compile_fail/latency_bad.cpp`.
- **`BankPair` moved with `Latency`.** The plan needs it complete and `node_type.hpp` includes `latency.hpp`, so it
  cannot be included back; `Ports` is forward-declared. No caller changed.
- **Beside today's plan.** `node_host.hpp`'s `ear::LatencyPlan` (with `internal`) stays until R2, so the plan by role,
  `LatencyFacts` and `workOut` are in `ear::latency_model`. R2 deletes `node_host.hpp`'s `LatencyPlan` and moves the
  three into `ear`.
- **`latency_spa.hpp` landed in R1** (pure addition, used only by its tests), so `ear_pipewire_tests` checks the
  model's combine against `spa_latency_info_combine` and `_finish` now. `fromSpa`'s none is an unfinished empty
  combination (`spa_latency_info_combine_start`'s sentinels); both directions saturate.
- **The roles' rule, fixed.** `onPair` counts on its own pair's path, both ways; `onBank` on what its bank tells and
  nothing else; a paired bank with no peer is a zero path through its pair (as today: an empty combination finishes
  at zero). `workOut` throws `std::logic_error` for a plan that does not fit the ports (a bank they lack, a pair
  backwards, a pair delay off the plan's pairs, an entry twice) or peers of the wrong count, so R2 calls it once at
  `carryLatency` (on the main loop, before any `param_changed`) and never first meets a bad plan inside a C callback.
- **R2's mapping keeps behaviour.** Today `internal` is keyed by bank: one entry on an output bank `B` is added to all
  of `B`'s told and counted through on every input paired with `B`; one on an input bank only to its own told. So R2
  fills: a declaration on an output bank that some pair ends at becomes `onPair` on every pair into it; one on an
  output bank in no pair, or on an input bank, becomes `onBank`. Today's only declaring type, `sendspin` (its feed on
  the unpaired `from`), is `onBank`. Counting a typed `declareLatency(from, to)` on its own pair only is a behaviour
  change for a bank with two pairs; R2 makes it only by saying so.
- **Told's order** is as 3.3 declared, one vector: the input side's ports (inputs then insideInputs), then the
  output side's.
- **`FrameClock`** divides the whole frame count once, truncating toward zero (`enteredRawNs` is
  `startRawNs + ns(offset - upstream.max)`), which is what each of today's four sites computes, so R3 changes no date.

### 3.6 Found building R2

What R2 built differs from 3.3 and 3.4 here; `docs/engine.md#latency-per-bank-pair` describes it.

- **One namespace, one signature.** `LatencyPlan`, `LatencyFacts` and `workOut` are in `ear` (`ear::latency_model` is
  gone, and so is `node_host.hpp`'s plan with `internal`); `node_host.hpp` includes `latency.hpp`. `workOut` keeps
  3.3's one signature: the harness passes none for every port rather than gain an overload.
- **A typed declaration is a path (review).** A typed `declareLatency(from, to)` names its path, and the plan carries
  that pair with the delay as its `onPair`. 3.5's mapping by bank alone dropped `from` for a code of the type's own
  (no pass-through, so no pair): its delay reached `to` only, never `from`'s told playback latency, so a source on
  `from` would date `Played` early, the class of the 3b.2 regression. For a code of the type's own the pair is added
  to `pairs`; with a pass-through it must be one of its pairs, or the node fails `configureFailed` at configure's
  return naming the pair (a declared path must exist: a delay on a path no audio takes would be told on banks it
  never touches). Typed declarations are keyed by path, not by output bank, so a mixer (two inputs into one output)
  declares each path, and a second for the same path is the error; the legacy one-bank declaration keeps its per-bank
  key and 3.5's mapping. No shipped type declares a typed path (`sendspin`'s is legacy), so nothing shipped
  changes.
- **A misfit plan fails configure (review).** NodeHost builds the plan at configure's return and works it out with no
  peers there, so a plan that does not fit fails the node `start-failed` with its reason; `drive()` carries the plan
  it checked. (First built, `carryLatency`'s throw came in `drive()` before the barrier's bound was armed, so a
  framework bug would have left the node starting with no bound.) Only a framework bug makes such a plan: each
  declaration is checked at its call, and no test can reach the check through a type.
- **The block works out first, then keeps.** `carryLatency` calls `workOut` before it stores the plan, so a plan that
  does not fit throws and is never kept; `param_changed` only changes a peer. The block before ignored a `beyond`
  naming no bank. It keeps peers as `std::optional<Latency>` (converted by `fromSpa` as heard) and what each port last
  told in frames; it tells a port only when that changes, so a port that tells zero still sends nothing, as before.
- **A negative peer is clamped before the series add.** `fromSpa` makes a negative total zero at the port, before a
  `beyond` or a delay is added; the block before summed in PipeWire's units first, so a peer reporting -100 frames
  behind a receiver's 480 told 380 and now tells 480. A behaviour change beside 8's, for peers no shipped node
  reports; `latency_spa_test.cpp` and the conformance test pin it.
- **Peers told in rate units** (the one change 8 allows): the block told its peers' units through, combined field by
  field; now `toSpa`'s rate units only, and a bank's paths combined in frames, so a sink's `upstreamFrames` is the
  exact most where peers mixed units (`docs/engine.md#sinks` says so).
- **The table is shared.** The rows moved from `latency_test.cpp` to `test/support/latency_table.hpp`
  (`ear_latency_table`), which `ear_core_tests` runs through `workOut` and `ear_pipewire_tests` through a real block.
  The conformance test links every port to a bare `pw_filter` that records what PipeWire sets on its port: a peer
  telling in mixed units (the least in ns, the most in quanta and frames) where the row gives one, else a listener
  with `port.ignore-latency`, which `pw_impl_port_recalc_latency` skips, so the port has no path but its told
  latency is still heard. It reads both bounds of every port's told latency on the wire (in rate units only), each
  output's downstream in the code, and each inside bank's published latency through a source (`SourceCycle`) or a
  sink's stamp (`upstreamFrames`). Each row expects some latency that is not zero (two rows gained a bank's own delay
  for it), so no row passes for a block that tells nothing. Every row runs, "two pairs into one output" included,
  though a pass-through refuses that shape: the block's rule holds for any plan.
- **The harness's side of section 7's comparison** is by construction now (it calls `workOut`); R7 keeps the sink
  delivery and companion-bank rules.
- **No other hand combine was found.** Outside the model, latency is only converted: `NodeHost`'s RTP receiver
  `latencyMs * rate / 1000` (milliseconds to frames, kept) and the four dating sites (R3).

## 4. Class 2: exceptions at the C boundary

### 4.1 The root class

PipeWire calls ear through C function pointers (loop events, timers, signals, invokes, and the listeners of filters,
proxies, the registry, the core and modules). An exception that reaches one unwinds through C frames, which is
`std::terminate`. ear has no single place that stands between them: each review found one more path and fixed it there.

- 6871832c59 (3a L-3a review): an input that threw cleared the inputs queued behind it.
- 4050e768eb (L-3b's review, folded into L-3c): a flush item that threw stranded its node.
- 07929a184d (L-4a review): a type's recipe, a legacy close and a configure could throw out of NodeHost ("nothing a type
  does thrown out of NodeHost").
- 0c8a0aecd3 (L-4b review): `FilterPortBlock::drive` threw on the main loop.
- e4b72cf661 (3b.1 review): `carryLatency` threw for a `beyond` entry naming no bank.

Today `node_host.cpp` has 20 `catch` sites, and `src/pipewire/` none. NodeHost catches during a drain and rethrows the
first exception to its caller (`node_host.cpp:1700-1716`, `1774-1781`), and its caller is a C callback.
`MainLoopExecutor::onEvent` (`main_loop_executor.cpp:38-46`) loses the rest of a batch when one item throws, and
`LoopTimer::onTimeout` (`:77-80`) lets a timer's throw through. `SinkConsumer` says "a throw is std::terminate on the
sink's thread" (`sink.hpp`). ear registers C callbacks at 21 places: `links.cpp:24`, `sound_cards.cpp:66`,
`handoffs.cpp:33,34,131`, `live_graph.cpp:41`, `link_proxy.hpp:31`, `main_loop_executor.cpp:19,63`,
`rtp_receivers.cpp:67,68,144`, `engine_companions.cpp:30,31`, `port_blocks.cpp:121,122,134,166,473` and
`engine.cpp:237,238`.

### 4.2 The fix

Two boundaries, each in one place.

**The engine boundary** (`src/pipewire/callback_boundary.hpp`, private to `ear_pipewire`). Every C callback ear gives
PipeWire is `guarded<&Class::member>`: a `noexcept` trampoline that calls the member and catches. A caught exception
goes to `Faults`, with where it happened (the callback's name, and `MainLoopActivity`'s node and call when one is open)
and the callback's owner when the class is a `FaultOwner`. The boundary's registration functions (`addEvent`,
`addTimer`, `addSignal`, `invoke`, and one `listen` per events type ear uses: filter, proxy, registry, core, module)
take only a `Guarded` value, which only `guarded<>` makes, so passing a raw function pointer does not compile.
`MainLoopExecutor` runs each item and each timer through the boundary, so one item's throw never drops the rest.

Data-loop callbacks (the filter's `process`, the request invoke's drain, `port_blocks.cpp:134,473`) are registered
through the same functions as `realtime<&Class::member>`, which requires the member to be `noexcept` and adds no catch:
realtime code never throws, and a throw there terminates, as today (`docs/engine.md#the-one-data-loop-and-invokes`).

**The type boundary** (inside NodeHost). A type's code is called at configure, start, stop, a recipe, a legacy close
and a sink consumer's begin, consume and finish. Each becomes one `callType(hook, fn)` returning
`std::expected<R, TypeFault>`, which records a broken rule first, times the hook (`ear.hook-slow`) and says what threw;
the call sites keep their facts (`start-failed` and the rest, `docs/writing-a-node-type.md#rules-the-framework-enforces`)
but lose their own `try`. A sink consumer runs on its sink's thread through the same function, so a consumer that
throws is a fact like a hook that throws, not a terminate (the owner's question, section 9).

**The guard.** `test/pipewire/callback_guard.cmake`, a CTest test built as `standard_output_guard.cmake` is: it fails
naming the file and line when any file under `src/` other than `callback_boundary.*` names `pw_loop_add_`,
`pw_data_loop_invoke`, `pw_loop_invoke` or `_add_listener(`, and it fails when it checked no file. The compile-time
half is `Guarded`'s type; the test-time half is the guard.

### 4.3 Interfaces

```cpp
// src/pipewire/callback_boundary.hpp (private to ear_pipewire)
namespace ear::pipewire {
/// An object whose own callback may fault: told when one threw, inside the boundary, never from the call that threw.
class FaultOwner {
public:
	virtual void faulted(std::string_view callback, std::string detail) noexcept = 0;

protected:
	~FaultOwner() = default;
};
/// Where every caught fault is reported (the engine holds one; main.cpp provides it).
class Faults {
public:
	/// `node` is MainLoopActivity's when a scope is open, else empty. Owned: after this, the owner's faulted().
	virtual void caught(std::string_view callback, std::string_view node, std::string_view detail,
	                    bool owned) noexcept = 0;

protected:
	~Faults() = default;
};
/// A C callback made from a member function: noexcept, catching, reporting. Only guarded<> makes one.
template <auto Member> class Guarded; // specialised on the member's class and signature
template <auto Member> inline constexpr Guarded<Member> guarded{};
/// A data-loop callback: the member must be noexcept; no catch is added.
template <auto Member> class Realtime;
template <auto Member> inline constexpr Realtime<Member> realtime{};
// MemberClass<Member>: the class a member pointer belongs to (a trait in the same header).
/// The only registrations ear makes; each takes a Guarded.
template <auto Member> spa_source* addEvent(pw_loop* loop, Guarded<Member>, MemberClass<Member>* self);
template <auto Member> spa_source* addTimer(pw_loop* loop, Guarded<Member>, MemberClass<Member>* self);
template <auto Member> spa_source* addSignal(pw_loop* loop, int signal, Guarded<Member>, MemberClass<Member>* self);
template <auto Member> int invoke(pw_loop* loop, Guarded<Member>, MemberClass<Member>* self, bool block);
// listen(): one overload per events table (pw_filter_events, pw_proxy_events, pw_registry_events, pw_core_events,
// pw_impl_module_events), each taking the table's callbacks as Guarded values.
} // namespace ear::pipewire

// src/core/node_host.cpp (private): the type boundary
struct TypeFault {
	std::string detail;               // "start threw: <what>", or the first rule broken
	bool rule = false;                // a std::logic_error a context call recorded
};
template <class Fn> auto callType(std::string_view hook, Fn&& fn) -> std::expected<std::invoke_result_t<Fn>, TypeFault>;
```

### 4.4 Migration

- **R4a** (behaviour-free; **built**, as `docs/architecture.md#the-callback-boundary` describes, with the changes
  below): `callback_boundary.*`; the 21 registrations move onto it; `MainLoopExecutor` runs items one
  by one through it; `Faults` is implemented in `main.cpp` as today's outcome made explicit: write the callback and node
  into the crash's last words, then `std::terminate()` (the handler `main.cpp:216` installs). The guard lands with it.
- **R4b**: the owner's answer (section 9) replaces R4a's `Faults` and makes blocks, engine companions and links
  `FaultOwner`s, routed to their node's `lost`.
- **R4c** (behaviour-free but for the consumer): NodeHost's type calls go through `callType`; `ThreadSink` calls the
  consumer through it and reports a fault as the sink's loss.

R4a follows 3b.4, since it edits `port_blocks.cpp`, `sound_cards.cpp` and `engine.cpp`; it must land before 3c, whose
loops and I/O executor would otherwise add callbacks outside it.

### 4.5 Found building R4a

What 4.3 declared that could not work as declared, and the least change made (the built boundary is
`docs/architecture.md#the-callback-boundary`; R4b and R4c build on it as it is):

- **28 registrations, not 21.** Section 4.1's count missed the engine log (`pw_log_set` of a `spa_log_methods` table,
  and alsa-lib's error handler by `snd_lib_error_set_handler`), called on any thread, the data loop among them, with C
  varargs no trampoline forwards: they are `realtimeFunction<>` values (the function itself `noexcept`) registered by
  `installLog<LogCallbacks{...}>` and `setAlsaErrorHandler`, so the boundary has no exception. It also missed the node
  and link info listeners
  (`pw_proxy_add_object_listener`: `sound_cards.cpp:77`, `rtp_receivers.cpp:177`, `link_proxy.hpp:32`), which
  `_add_listener(` does not match, and `engine.cpp`'s two data-loop invokes (`pw_data_loop_invoke`, the denormal flush
  and the scheduling read). So `listen` has `NodeCallbacks` and `LinkCallbacks` too, both invokes are `realtime<>`, and
  the guard also refuses `_add_object_listener(`, `spa_loop_invoke` and `spa_loop_utils_add_`. The scheduling read
  formatted its string on the data loop (`std::to_string`); it now reads the policy and priority there and formats them
  on the caller.
- **`node` could never be filled.** A fault reaches the boundary after every `MainLoopActivity::Scope` has unwound, so
  "MainLoopActivity's node when a scope is open" was always empty for a throw out of NodeHost, and NodeHost rethrows the
  first exception of a drain or flush after the scope it was thrown in has ended. The second parameter is `where`
  (MainLoopActivity's "<call> <node>"): a scope a throw ends keeps its place (`takeThrown`, the innermost scope's;
  `forgetThrown` when a scope begins or returns or a main-loop callback begins, but never while an exception unwinds,
  so a callback a destructor makes during the unwinding keeps the place), NodeHost keeps the place with the exception it rethrows
  (`KeptThrow`) and puts it back as it rethrows (`putThrown`), and a callback made inside an open call is placed in that
  call. Off the main thread `where` is empty.
- **`Faults` is public, the boundary private.** `main.cpp` provides `Faults` but cannot see a private header: `Faults`
  (and `faultLine`, the one wording of the crash's line) is `ear/pipewire/faults.hpp`; `FaultOwner` stays in
  `callback_boundary.hpp`, and its `faulted` takes the detail as `std::string_view`. The engine holds it as
  `EngineSettings::faults`; since a trampoline has only its object's pointer, the boundary keeps the installed one
  process-wide, and `Engine::create` fails while another engine holds it (a second would take the first's reports, or
  remove them as it went); with none installed it writes the line to standard error and ends the process.
- **The crash's line goes to the real standard error, in order.** Written to descriptor 2 it would go through the
  capture's pipe, whose thread could take it and lose it as the process dies; `DescriptorCapture::lastWords(line)`
  writes out what is queued and what the pipes hold, then the line whole on the real standard error, and `main.cpp`'s
  `Faults` calls it before `std::terminate()`.
- **The registration functions** are one template each, constrained to a `Guarded` or `Realtime` of the right signature
  with the object typed as the member's class, rather than an overload per kind; `listen` takes its table as a template
  argument (`listen<FilterCallbacks<C>{.stateChanged = guarded<...>, .process = realtime<...>}>(filter, hook, self)`),
  so each PipeWire table is a constant, and a filter's `process` entry takes only `realtime<>`, every other entry only
  `guarded<>`. The loop's kind is a type: event, timer and signal sources and an invoke on a `pw_loop` take only
  `guarded<>`, and an invoke on the data loop takes a `DataLoop` and only `realtime<>`. An invoke's member takes nothing and returns `int` or nothing. A `Guarded` holds nothing but its member,
  so any one is the boundary's own; what cannot compile is handing a registration anything else. The executor's items
  run through `shield(callback, work)`, the boundary's catch for work that is not a callback of its own.
- **The guard skips `tests/` folders** as well as exactly `pipewire/callback_boundary.hpp` and `.cpp`:
  `src/pipewire/tests` drives PipeWire directly (the spikes, probes and waits). Its spellings are every way to hand a
  library a callback, not only 4.2's four: also `spa_loop_add_source`, `pw_loop_locked`, `spa_loop_locked`,
  `pw_work_queue_add`, `spa_hook_list_append`, `pw_stream_new_simple`, `pw_filter_new_simple`, `SPA_INTERFACE_INIT`,
  `SPA_CALLBACKS_INIT`, `spa_callbacks`, any `spa_*_methods` or `pw_*_methods` table, `pw_log_set`,
  `snd_lib_error_set_handler`, and a listener whose parenthesis is on the next line.

## 5. Class 3: tests that depend on the host

### 5.1 The root class

A test that needs a rare or timed event either waits for the host to produce it, or reads a value one hop before the
change it waits for has propagated, or judges with a margin sized to the host it ran on. `docs/testing.md` gained the
rule "a test makes the rare event it needs" (with S5), but the suite was never audited and nothing enforces it.
Evidence: S2 (1fc5a35426, stepped-back positions, then made serial), S5 (5553e1fb8f, waited for 500 cut-short cycles:
9 on CI, 5,000 on a workstation), the stall test requiring block skips (154645370a), the sendspin clock tests (67d067abed,
5bf75b4d1e, 2af1cfd56e, 8c8e6f25ae, 37c69e6945, 55dd4cdaa6), the levels tests' RMS against a fixed 0.02 (5bf75b4d1e),
E2(3) and J(5) (889ccc50e5), and the beyond-latency test reading one probe (40be60af04).

### 5.2 The fix

Three tools, a guard, and the audit. The tools and the guard are built (R5a), as amended in 5.4; `docs/testing.md#the-tools`
is their home, and this section stays only as the audit's starting point.

- **Inducers** (`src/pipewire/tests/inducers.hpp`): `StallingCode`, an `AudioCode` that sleeps a planned length inside
  `process` at planned cycles and counts what it did (lifted from S5 and `port_block_test.cpp:1780-1795`), and
  `CycleLedger`, which records each cycle's position, `xruns` and `discontinuity` as the block saw them. A test that
  needs N stalls makes N; a test that judges exactness judges over the ledger's clean cycles and waits until it has the
  number it planned. A loaded host then only makes the test take longer: the event it waits for is "N clean cycles
  judged", never "1 s passed".
- **A settled engine** (`EngineHarness::quiesce`): `pw_core_sync` round trips until one completes with no block
  retelling latency (`FilterPortBlocks::latencyRetells()`, a main-thread counter), then one complete cycle of each
  block named. It replaces waiting for one probe's sighting after a plan or a link changed.
- **Statistics from the count** (`test/support/statistics.hpp`): a statistical check takes its bound from the samples
  it measured (`noiseRmsWithin(rms, frames, sigmas)`), never a fixed margin.
- **The guard** (`test/sleep_guard.cmake`, a CTest test): every `sleep_for`, `usleep` and `nanosleep` under `src/*/tests`
  and `test/` is listed in `test/sleeps.txt` with its role, `induce` (the sleep is the event) or `negative` (a bounded
  look for something that must not happen, taken after an event that would have caused it); an unlisted one fails.
  `WARN` followed by going on is refused the same way: a wait that times out fails, naming its event.

### 5.3 The audit

Each test that waits on or counts what the host does, and how it changes (R5b: `ear_pipewire_tests`; R5c: `sendspin`;
R5d: the system tests).

| Test | Waits on or counts | Change |
|---|---|---|
| `engine_test.cpp` "A link carries a source block's ramp ... sample for sample" | No xrun in its window | `CycleLedger`: exact over clean cycles, until a planned count |
| `port_block_test.cpp` "Latency is carried per bank pair ...", "A companion's latency beyond an inside bank ...", "A following card's latency reaches ..." | Retell reaching each probe | `quiesce` before reading |
| `port_block_test.cpp` "After data-thread stalls past 10 ms ...", "After a 3 s data-thread stall ..." | Already induced | Onto `StallingCode`; listed `induce` |
| `spikes_test.cpp` S2 | Step-backs and cut cycles the host made | Induced stalls (`StallingCode`); `RUN_SERIAL` kept for its busy core |
| `spikes_test.cpp` S5, `source_test.cpp` "... cut short by a probe sleeping ..." | Already induced | Onto `StallingCode` |
| `sink_test.cpp:592`, "stopped for a while" | 50 ms standing for cycles run | Wait for N cycles in the ledger |
| `source_test.cpp:103,143,197` | A writer still waiting after a wall time | `negative`, taken after N cycles ran |
| `sendspin/tests/feed_test.cpp:70` | 50 ms past the writer's second look | `negative` (kept; its comment says why) |
| `sendspin/tests/clock_test.cpp:233-234` | The tracking loop reaching the raw rate, else `WARN` and go on | Fail naming the event; the wait itself is ear's own event |
| `sendspin/tests/clock_test.cpp` "within 1 ms" cases | Bad cycles counted on the host's schedule | Judge over the ledger's clean cycles to a planned count |
| `test/system/sendspin_*` ramps ("1 s of the 1.5 s played") | A late join under load | Wait for 1 s of judged frames, as many seconds of play as it takes |
| `test/system/sendspin_floods_test.cpp`, renewal every 500 ms | A race between renewal and closing, sized to the host | Renew on the transport's close of the last socket (an event), not a period |
| `test/system/sendspin_floods_test.cpp`, the thread peak sampled 3,000 times at 1 ms | A sampled peak can miss a breach (passes by luck, never fails by it) | `negative`; the bound is proven where it is enforced, in the transport's own tests |
| `test/system/white_to_file_test.cpp:46`, `graph_file_test.cpp:101` | A 1 s recording window | Wait for the WAV to hold the frames the check needs (its size, polled as an event) |
| `test/system/control_port_test.cpp` levels and RMS, the sendspin client's recorded channel | RMS margins | `noiseRmsWithin` everywhere |
| RTP receiver tests (`rtp_receiver_test.cpp`, `test/system/rtp_test.cpp`) | No loss or reorder on 127.0.0.1 under load | Judge over the ledger's clean cycles and the receiver's own counters |
| `stall_watch_test.cpp`, `main_loop_executor_test.cpp:217` | A real stall | Already induced; listed `induce` |

`docs/testing.md`'s rules gain the ledger, `quiesce`, statistics from the count and the guard, and the table of what
each program proves keeps its rows.

### 5.4 Found building R5a

- **Where the inducers live.** `inducers.hpp` is in `test/support/` (the target `ear_inducers`, namespace `ear::test`),
  not `src/pipewire/tests/`: R5c's `sendspin` clock tests and R5d's system tests need the ledger too, and nothing in it
  is PipeWire's. The planned stall is its own value and actor (`StallPlan`, `Stalls`), which `StallingCode` uses:
  S5's overruns and the source's cut-short cycles are made inside probes (a `ProbeCode`, not an `AudioCode`), so the
  stall had to be usable outside `StallingCode`. `CycleLedger` records what the design says plus the frames, both
  clocks' start and the rate (the clock tests judge those), and marks the cycle that stalled on purpose so the one after
  it is not clean.
- **`quiesce` has no `EngineHarness` to live on and no counter in `ear_pipewire`.** No class of that name exists (the
  engine tests share free functions and a `Waiter`, and `port_block_test.cpp` has its own), and
  `FilterPortBlocks::latencyRetells()` would edit `port_blocks.*`, which 3b.4 is changing. It is
  `ear::pipewire::testing::Quiescence<Waiter>` (`src/pipewire/tests/quiescence.hpp`), which counts every port info
  with changed params heard through the registry: a block's retell reaches PipeWire as exactly that, and so do the
  latency PipeWire sets on the peers and a probe's or a companion's change, which a block counter would miss. It waits
  for two quiet round trips, not one (the engine shares its main loop with ear, so a trip can end while PipeWire holds
  work for a later turn), and "one complete cycle of each block named" is given as each code's cycle count (moved on by
  two). R2 may still add the counter; nothing here needs it.
- **Two more roles of a sleep.** Besides `induce` and `negative`: `poll` (a pause between looks at the event, which
  decides: `driveNow`, the test server's wait for a player role the library gives no event for) and `pace` (the clock
  of a stand-in that keeps real time: the RTP test sender, the stall watch's heartbeat, the invoke storm, the server's
  play-out). The audit's inventory is listed as `luck` (a sleep or `WARN` that stands in for an event or needs the host
  to be quick) and `owed` (a `WARN` that skips or reports), each naming its step.
- **The `WARN` rule is enforced by the same guard, statically.** A Catch reporter or listener failing any run with a
  warning would fail the suite at once: `clock_test.cpp` reports with `WARN` on every run, and two tests skip under
  root with one. So `WARN`, `CATCH_WARN` and `CHECK_NOFAIL` are listed in `test/sleeps.txt` like sleeps, as `luck` or
  `owed` only: none can be added unlisted, and R5b to R5d remove the listed ones.
- **What the table above misses.** `source_test.cpp:143` and `:197` are luck-waits (50 and 200 ms standing for "the
  writer is waiting"), not negatives; `:103` is a negative. The guard also reads `src/core/testing/`,
  `src/core/include/ear/testing/` and `test/support/sendspin_server.py`; and `ear_output_tests`
  (`output_writer_test.cpp`'s 50 ms, the stall watch's) and `ear_folder_tests` (a skip under root) have entries the
  table lacks: they are given to R5d. `sleeps.txt` is the inventory; this table is not kept in step with it.

### 5.5 Found building R5b

`ear_pipewire_tests` has no `luck` or `owed` entry left in `test/sleeps.txt`; what each test now waits for or judges is
in `docs/testing.md#making-the-rare-event`.

- **S5 counted the source rule as a fault.** Under load (5 to 6 on 4 cores) it failed 2 runs in 20: a host stall over
  42 ms made a position jump beyond B's 2,048-frame ring, which the source rule makes a new clock (nothing skipped,
  played on from where the ring is), and the test counted that as drift and as a forbidden jump. B now records each new
  clock, the model re-anchors there, D judges the frame B played at its position in that cycle, and the gate is that
  every jump beyond the capacity, and nothing else, was a new clock to B. A planned stall cannot make such a jump on
  every host (a fast one catches up by running every missed cycle: `port_block_test.cpp`'s stall test), so new clocks
  are recorded, never required.
- **"The clock did not jump here" is read from the cycles, three ways.** The sink's take-up compares the block's own
  `xruns` at the code's first cycle with its last before the stop (the block counts them with no code driven); the
  port block's take-ups ask whether the listener downstream heard consecutive positions there; the engine's ramp test
  judges across a cycle boundary only on its recorder's clean cycles. Each repeats to a planned count of judged
  take-ups or cycles (2, 1 and 5, 32), and what holds across a jump too is checked every time. The sink's two `WARN`s
  became one stronger check: the sink's contiguous stretches equal the code's ledger's, jumps included.
- **The hold ends on the test's release.** `Holding` held 8 ms at most ("under max_resync's 10 ms"); engine.conf's
  resync.ms already lies beyond any stall, so the cap guarded nothing and only made the retry. It now holds until
  released (5 s only fails the test); a hold or a host past the next cycle's start makes a jump, judged as above.
- **"Stopped for a while" needed a second block.** A stopped block runs no code to count cycles with, so a block of
  its own counts 8 graph cycles; the gap keeps the rounds' stretches apart in position.
- **The writer's wait needed a product seam.** Nothing outside `EventSources` could see that a writer was waiting.
  `EventSources::waitsBegun(source)` (the only product change) counts the waits begun, each past the writer's last
  look at the ring; `source_test.cpp` waits for it before it plays or closes. Its three sleeps are gone (none is left
  as a negative: once the writer waits, only a play, the close or its patience can end it), and so are the checks that
  leaned on them (`took >= 40ms`, `>= 150ms`).
- **More than the table named.** `port_block_test.cpp`'s first fade-in read `*frameAt(...)` for a position a jump can
  skip (undefined behaviour, not a failure); it is judged as the second is.
- **S5 part A on the real block had the same root class.** `source_test.cpp`'s part A required no jump past its
  8,192-frame ring, no drift and no shortfall, whatever the host did. It is now judged cycle by cycle by the source
  rule: Played's count moves by each cycle's jump and frames; a jump past the capacity is a new clock (recorded, never
  required; every such jump, and nothing else, a new clock); and a cycle short of frames is excused only when the
  writer had not written them by the cycle before, by its own count of frames written, which the block's code logs.
  The listener is judged on the cycles the source played whole and unfaded, against the stream frames its count says
  it played, to a planned 1,000 of them. No item of the audit is left unowned.
- **What excuses a jump is checked against the raw clock (review).** Re-anchoring alone gated only the test's own
  arithmetic: a driver that jumped 64 cycles after a cut-short cycle would have passed as a "new clock". S5 and part A
  now record each cycle's raw time (B's own read at entry; the block's `startRawNs`) and require every forward jump to
  be time the raw clock shows was lost since the last cycle's end, within two cycles and the driver's 1 ms clamp
  (idle: 0 and 5 frames at most). The writer's wait ended by the close is now told apart from one ended by its
  patience (`waitsWoken`), and the year-long patience is checked directly (`waitDeadline`).

## 6. Class 4: PipeWire properties

### 6.1 The root class

ear sets PipeWire properties in six places: `engine.conf`, `contextProperties` (`engine.cpp:131-145`), the port block
(`port_blocks.cpp:127-129`, `256-257`), the card adapter (`sound_cards.cpp:53-58`, plus the test seam's `extra`), the RTP
module's arguments (`rtp_receivers.cpp:35-47`) and links (`engine_companions.cpp:83-85`, `links.cpp:99-100`). Each
key's effect lives in PipeWire's source, and its couplings were learned when a test failed: `resync.ms` and the clock's
reset and step back (c1bb9d246b); `quantum-limit` and the ALSA buffer (3b.4); force and lock properties and
`clock.duration` (D-3b12); `context.properties` overwriting `default.clock.*`; `clock.power-of-two-quantum` rounding 480
to 256; `node.group` on a card stopping it from starting alone; `node.driver = false` reasserted by the ALSA node;
`context.num-data-loops` and the one data loop; `sess.latency.msec` and the `beyond` entry (rtp-source reports zero).
The reasons are kept, but in comments and prose in four places, with no single list of keys and nothing that notices a
key added without a reason or a test.

### 6.2 The fix

One table, `src/pipewire/engine_properties.hpp`, and four checks.

- **Compile time.** Each builder takes its keys from `property(Home, "key")`, a `consteval` lookup that fails to
  compile for a key the table does not list for that home. `forbidden` lists what no object may carry
  (`node.force-quantum`, `node.force-rate`, `node.lock-quantum`, `node.lock-rate`; in `engine.conf`, `default.clock.*`,
  `clock.power-of-two-quantum` and `cpu.zero.denormals`; on a card, `node.group` and `node.driver`); the card seam
  refuses from it.
- **Against the live engine** (`engine_properties_test.cpp`): each row's value as PipeWire holds it on the made object
  (`pw_context_get_properties`, the internal clock's and a card's node properties through the registry, the receiver's
  node), so a PipeWire that overrides or drops one fails.
- **Each coupling pinned.** Every row names the test that pins its coupling (`pinnedBy`); a CTest test lists the test
  names and fails for a row whose test does not exist.
- **Against the doc.** `docs/engine.md`'s `engine.conf, key by key` section becomes "Properties ear sets", one table of
  key, where, value, why, couples to, pinned by; a CTest test reads the table and fails when its keys and the code's
  differ (as `ear_doc_example_*` check the worked example).

### 6.3 Interfaces

```cpp
// src/pipewire/engine_properties.hpp (private to ear_pipewire; its tests include it)
namespace ear::pipewire {
enum class Home { engineConf, context, internalClock, block, blockPort, card, rtpReceiver, link };
struct PropertyRow {
	Home home;
	std::string_view key;
	std::string_view value;     // as set, or how it is computed ("the block size")
	std::string_view couplesTo; // what else it changes, in PipeWire's terms
	std::string_view anchor;    // the docs/engine.md heading that says why
	std::string_view pinnedBy;  // the Catch test name that pins the coupling
};
inline constexpr std::array<PropertyRow, /*N*/> engineProperties{/* one row per key */};
inline constexpr std::array<std::pair<Home, std::string_view>, /*M*/> forbidden{/* home, key or key prefix */};
/// The key, if the table lists it for `home`; does not compile otherwise.
consteval const char* property(Home home, std::string_view key); // built: returns a PropertyKey (6.5)
} // namespace ear::pipewire
```

### 6.4 Migration

**R6**, after 3b.4 (which settles `quantum-limit` and the seam's refusal): the table, the six builders onto
`property()`, the live and doc checks, `docs/engine.md`'s section rewritten as the table, the `pipewire-engine` skill
pointing at it, and the comments in `engine.conf` cut to a pointer per key.

### 6.5 Found building R6

The table is `src/pipewire/engine_properties.hpp`; how it is checked and why each key is set is
`docs/engine.md#properties-ear-sets`. What changed from 6.3, and what building it found:

- **No drift.** The live check reads every row back from the object PipeWire 1.4.2 holds (through the context's own
  objects: `pw_context_get_properties`, `pw_impl_node_get_properties`, `pw_impl_port_get_properties`, a link's info,
  the module's parsed arguments) and every value held as the table says. No behaviour changed. One key is consumed
  rather than held: the RTP stream's `adapter.auto-port-config` is read by the stream's own adapter on the module's
  connection and never reaches the engine's node, so the live check reads it from the module's `stream.props` and
  checks its effect (the `receive_AUX0` port) on the node.
- **Setters 6.1 missed.** Port properties (`format.dsp`, `port.name`, the `blockPort` home 6.3 already had), the card
  seam's `clock.name` (`clocklessCardProperties()`, a test seam but a property ear sets), `config.name` among the
  context's, and the RTP receiver's `stream.props` contents, which are the receiver node's own properties. The
  internal clock's arguments are `engine.conf`'s `context.objects`, so `engineConf` is only its `context.properties`.
  No setter outside `src/pipewire/` sets one.
- **Three more homes.** `cardSeam` (what the seam may add: it refuses a key forbidden on a card or one ear sets on
  every card, which is the old list of eight and every other card row too), `rtpStream` (the keys inside
  `stream.props`) and `companionLink` beside `link`: a route link names ports by global id, a companion's link names
  nodes by id and ports by name, the same keys with different values. `homeName()` and `allHomes` give the doc check
  each home's name.
- **`forbidden` holds `node.link-group` on a card** (the seam already refused it), and the force and lock keys on
  each node home rather than on "any object". `priority.driver` is not forbidden on a card, since the card sets it;
  the seam refuses it as a card row.
- **`property()` fails by calling a function that is not `constexpr`**, so GCC names it in the error; four compile
  checks (a control, an unlisted key, another home's key, a forbidden key) pin it. The table's `static_assert`s add
  what a lookup cannot see: a key listed twice for one home, a row forbidden on its home, a key in both `engine.conf`
  and the context (the file would overwrite `Engine`'s), a seam key a card sets.
- **`engine.conf` is checked as a file too.** The live check proves the table's rows are held; the conf check reads
  the file with PipeWire's own parser and fails for a key there the table lacks, a value it does not hold, a forbidden
  key or a second `context.objects` entry.
- **`clock.power-of-two-quantum` had no pinning test** (every engine test ran at 256): "A block size that is not a
  power of two runs as itself" runs the engine at 480.
- **The doc table's columns are checked against the code's.** Value and pinned by must equal the code's (a value as
  set is one backticked span, a computed one prose); why must link its row's anchor, and every anchor must be a heading of the page. The
  forbidden keys have their own table, "Properties ear never sets". `engine.conf`'s modules and libraries, which are
  not properties, moved to `docs/engine.md#modules-and-libraries`.
- **The review closed the routes around the table.** `engine.conf` may hold only its four sections and modules
  without arguments (libpipewire reads properties from `filter.properties`, `stream.properties`, `node.rules` and
  more); no made object may hold a forbidden key (but `node.driver` on a card, which the ALSA node publishes); a
  source guard (`test/pipewire/property_guard.cmake`) refuses a literal or macro key to every properties setter
  under `src/`, since `property()` alone was a convention; the RTP receiver's rows are read from its node where it
  holds them (`remote.name` and `stream.props` only from the module's arguments), `adapter.auto-port-config` by the
  node's exact ports, and links are found by their nodes' ids with each held port id read back as its name. A
  forbidden key fails `property()` by its own function (`forbiddenOnThisHome`). `couplesTo` left the code: what
  a key couples to lives in `docs/engine.md`'s table alone, beside its reason, since no check reads it. The block's
  `media.name`, which `pw_filter_new` derived from its name, is set from the table.
- **A key is a type, not a string a text guard follows (re-review).** Scanning text to prove where a key came from
  had holes of one class: a `//` inside a string hid the rest of its line, the `_checked`, `va`, `update` and `add`
  writers and `spa_dict_item` aggregates slipped by, and any plain name passed. So 6.3's `property()` returns a
  `PropertyKey` (a `const char*` only the table, and `PropertyKey::seamChecked` for the card seam's keys after the
  refusal, can make), and `engine_property_set.{hpp,cpp}` (`newProperties`, `setProperty`, `ModuleArguments`,
  `loadModule`) is the only code that writes properties or loads a module with arguments, taking keys only as
  `PropertyKey`s. The guard shrank to "no PipeWire property writer outside that file", matching whole identifiers by
  prefix, with literals emptied before comments are cut. The live check also requires the RTP module's arguments to
  be exactly the table's receiver keys, the card's own `node.driver = true`, and each companion's links to leave from
  every one of its ports.

## 7. Class 5: the harness re-implements the engine

### 7.1 The root class

`TypeHarness` runs a real NodeHost over fakes, and where a fake must act as the engine does it re-implements the rule:
latency (`node_harness.cpp:495-548`), sink delivery (the harness's sink never splits a chunk, so 3b.1 could not show in
a type's tests), and an engine companion's bank (found by its ports, 3b.3). Each engine fix had to be repeated in the
harness, and a rule fixed in one alone leaves a type's tests passing on behaviour the engine does not have. D-3b4 (every
source rule in core's pure `SourceRing`, which the engine and the harness both wrap) is the pattern that works.

### 7.2 The fix

Every rule the harness models is a pure core function both sides call, and a conformance test runs one set of rows
through both:

- Latency: `workOut` (class 1).
- Sink delivery: `ChunkCutter`, a pure core function that turns one stamped cycle and a ring's free run into the calls
  a consumer gets (position and start times advanced by `FrameClock`). `ThreadSink` and the harness's sink both call
  it, and the harness gives its sink a ring size (`NodeHarness::sinkRingFrames`) so a type's tests see split chunks.
- An engine companion's bank: `NodeHost` keeps it (`EngineAsk`), and the harness reads it from there instead of
  matching ports.
- Conformance (`ear_pipewire_tests`): the latency table's rows through a real block and its probes, compared with
  `workOut`'s answer and with the harness's; a split chunk through `ThreadSink` and through the harness's sink.

```cpp
// src/core/include/ear/core/sink.hpp, beside Chunk
/// The calls one stamped cycle becomes when `contiguous` frames remain before the ring's end: one, or two with the
/// second's position and start times advanced by the first's frames.
struct ChunkCut { ChunkStamp first; std::uint32_t firstFrames; std::optional<ChunkStamp> second; std::uint32_t secondFrames; };
ChunkCut cutChunk(const ChunkStamp& stamp, std::uint32_t frames, std::uint32_t contiguous, FrameClock clock) noexcept;
```

**R7** (after R2 and R3): `cutChunk`, `ThreadSink` and the harness on it, the harness's ring size and companion bank,
the conformance tests.

## 8. Order

Each step is one mergeable change, green on every test and on `make format-check`, and changes the library pages it
makes stale.

| Step | What | When | Behaviour |
|---|---|---|---|
| **R1** | `latency.hpp` (the model, `FrameClock`), its table tests; `Latency` moves there | Now | None |
| **R5a** | `inducers.hpp`, `quiesce`, `statistics.hpp`, the sleep guard with today's sleeps listed | Now | None |
| **R2** | The block and the harness on `workOut`; `latency_spa.hpp`; `LatencyPlan` by role | After 3b.4 | Peers told in rate units (nobody reads them) |
| **R3** | Dating through `FrameClock` | After R2 | None |
| **R6** | The property table and its four checks; `docs/engine.md`'s table | After 3b.4 | None, unless the live check finds a drift |
| **R4a** | The callback boundary, behaviour-free; its guard (built, 4.5) | After 3b.4 | None |
| **R4b** | The owner's answer on faults | After R4a and the answer | Section 9 |
| **R4c** | NodeHost's type calls through `callType`; a sink consumer's throw as a fact | After R4a | Section 9 |
| **R5b-d** | The audit, by executable (5.3) | After R5a | None |
| **R7** | `cutChunk`; the harness's ring size and companion bank; conformance tests | After R3 | None |

All before 3c, whose sink-based `sendspin` takes `enteredRawNs` for its upstream dating (3.1) and whose loops are born
behind the boundary. 3b.5 to 3b.8 touch none of these files' rules and may interleave.

Each header lands with its step: R1 landed `latency.hpp` and `latency_spa.hpp`; the boundary's registrations touch
`node_host.hpp` and the files 3b.4's review is editing.

## 9. What an operator or a type author sees

Only class 2 changes what anyone sees, and only when ear or a type has a bug. That is the one question.

**Q-RC1 (answered: C). When ear's own code or a sink consumer throws inside a callback, what happens?**

*Background.* Code ear runs from PipeWire's callbacks (a block's events, an engine companion's, a link's, the main
loop's posted work and timers, a sink's thread) can throw only through a bug. Today that ends the process
(`std::terminate`, with the last records written out); a container restarts it, and every node restarts with it. A
type's hooks (configure, start, stop) already fail only their node (`start-failed`), but a type's sink consumer that
throws still ends the process.

*Scenario.* A future bug makes a card's engine companion throw while handling its node's info. Today ear exits and all
nodes, including an unrelated `rtp` → `card` route, go silent until the container restarts it (seconds), with the
crash's last words on standard output. Alternatively, the card node alone fails `lost` with "internal error in
card's companion: ...", the other routes play on, and a diagnostic record says what threw.

*Options.*

- A. Keep ending the process for every fault (R4a as built): simple and safe, since no state a bug left half-changed is
  used again; costs every route on any bug.
- B. Fail only the owning node for every fault, ear's own core included, and write `ear.internal-error`: keeps the
  rest playing; risks running on with NodeHost's own state half-changed when the fault was in core.
- C. Split by owner: a fault in an adapter's callback (a block, an engine companion, a link: state it alone holds) or
  in a type's sink consumer fails that node `lost` (the consumer: `start-failed`, as a hook that throws) and writes
  `ear.internal-error`; a fault in NodeHost or anything with no owner writes `ear.internal-error` and ends the process.

*Recommendation: C.* An adapter's state dies with its node, so failing the node is clean, and a type's consumer is
treated as its hooks already are. NodeHost's invariants cannot be trusted after an exception mid-update, so ending the
process stays the safe answer there; the new record makes it diagnosable. Under C, `ear.internal-error` (fields
`callback`, `node`?, `detail`) is added to `docs/output-records.md` and `output-records.json`, and
`docs/writing-a-node-type.md#sinks` says a consumer that throws fails its node.

*The owner's answer (2026-11-09):* "Split by owner (Recommended)", option C. Step R4b builds C.

