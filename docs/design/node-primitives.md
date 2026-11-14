# Node primitives: slice 3c of the node framework

Status: designed, not built; **re-planned against the code at 46b96c788c (2026-11-14) in section 0, which binds
where it and a later section differ**. Sections 1 to 18 are the design of 2026-11-09: their reasons, invariants,
failure table and rejected alternatives stand; their interfaces and their cut (13) are amended by section 0. Slice 3c
follows slice 3b (companions, built: `docs/architecture.md#companions`) and comes before slice 3d (`reconfigure.md`)
and slice 5 (`control-plane.md`). It gives every node type what `sendspin` had to build for itself (the node handle,
loops with their states, the I/O executor, stores, claims, recordings opened off the main loop, the carried
sendspin-cpp log patch), and moves `sendspin` onto the type's class, its companions and loops, retiring the legacy
adapter. The primitives' part of slice 3d (claims swapped in place, `whenReady`, a sink paused while a library is
swapped, `sendspin` changing in place) is `reconfigure.md`'s. The timing model's part of slice 3c is
`timing-model.md`'s: T1 to T4 are built, S3c.6 moves `sendspin`'s honest wrapper onto its source (section 0.5, as
that page's section 6 says), and T5 (the set and tracked converters) comes after S3c.6 with its first user. The built
system is described in `docs/architecture.md` and `docs/writing-a-node-type.md`; this page cites them for what exists.

[V] = verified in the file named, as of 2026-11-09 (files of the pinned sendspin-cpp tree, `.cache/deps/sendspin-src`,
by their path in it; PipeWire 1.4.2's source as `pipewire-1.4.2/<path>`); [U] = unverified, with the step or test that
checks it (section 18).

## 0. Slice 3c as built against today's code

Written 2026-11-14 against 46b96c788c, after slice 3b (companions, their counters, wire and badges), the timing
model's T1 to T4, cycle facts, `SampleRate`, the callback and sleep poisons, the `graph { }` format and the editor's
knobly node text. [V] here means read in that tree. This section is the plan; sections 1 to 18 keep the reasons.

### 0.1 What the code already gives, and what it changes here

| This page assumed (2026-11-09) | Today [V] | So |
|---|---|---|
| `Wake` and its eventfd are still to be made, and `EventWake` moved into `ear_runtime` (6.2) | `Wake` is core's (`ear/core/source.hpp`: `ring`, `descriptor`, `answer`, drain then clear), implemented on `EventSources`' eventfd in `ear_pipewire`; a source's `clockWake()` is built (T3) | `Loop::onWake(Wake&)` needs only core's interface, so nothing moves. Types still get no wakes of their own (G23) |
| `sendspin`'s source clock, the source's skip rule, `SourcePlayed` and `clockWake` are still to be built | Built (T3, `docs/writing-a-node-type.md#a-sources-clock`). `cx.source(bank, shape, std::unique_ptr<SourceClock>)` takes the clock, and `SendspinCode` is already T2's honest wrapper on today's feed | S3c.6 moves a wrapper that is already proven; it does not write one |
| A source's latency declared by hand (`declareLatency("from", feed)`) | A source declares `Latency{0, frames}` beyond its bank itself (`SourceShape`); the plan is by role and checked at configure (`docs/engine.md#latency-per-bank-pair`) | The legacy declaration goes with the feed. The latency told on `from` stays the same when the source's frames are `feedFramesFor` |
| Sizes written as `cx.format().rate / 2` (4.1, 4.2) | `AudioFormat::rate` is a `SampleRate`, which has no arithmetic (`docs/engine.md#dating`) | Every size is `FrameClock(cx.format().rate).frames(duration)`; 4.1 and 4.2 are corrected |
| Readiness has four steps written for this slice (6.1) | NodeHost already keeps two kinds of later readiness by hand: each engine companion and each sink's consumer, each with its own deadline timer, `Deadline` branch, `waitingFor` kind and failure text (`node_host.cpp`: `armDeadline`, `deadlinePassed`) | One abstraction, extracted first (0.2, A) |
| Jobs on the I/O thread check the configuration's generation themselves (6.6) | Every cross-thread report of a configuration already lands through its `Channel` (`node_host.cpp`: `say`, `lost`, `threw`, `sinkBegunCall`, `sinkClosedCall`, `handoffClosedCall`, `closedCall`), which is detached when the configuration ends | Completions, loop closes and the leaked record ride the `Channel` (0.2, B) |
| An RTP receiver claims `udp:<port>` as a special case (6.8) | RTP is the first module-run converter, and NodeHost names no RTP field (`ModuleRunShape`, `moduleRunDelay`, `docs/engine.md#module-run-converters`) | A companion's claims come from its shape, as its delay does (0.3) |
| `CreateOf::recording` returns an open `int` inside `configure` (6.10) | `file` opens synchronously in `configure` (`cx.openRecording`, marked "3a interim") and hands the descriptor to its `WavWriter` | A descriptor cannot exist before the file is opened, so the request returns a handle whose descriptor is valid from the consumer's `begin` (0.3) |
| `NodeHandle::log` needs a log entry type | `LogEntry` (`source`, `topic`, `node`, `cut`) and `LogSink` exist, marked "the seam for slice 3c's node-facing log" (`ear/core/log.hpp`) | The handle fills `node`; no new record kind |
| `waitingFor` gains four kinds | The wire's enums are open (`docs/control-port.md#protocol-and-features`), and `web/src/protocol.ts` types `kind` as the four names or any string | Each kind lands with its step, together with `docs/control-port.md`, `test/protocol/lifecycle.json` and `protocol.ts`; no feature name |
| Core's `NodeHost` tests are on the class SPI | `node_host_test.cpp` builds its test types on the legacy SPI (54 uses of `StartContext`, `StartResult`, `RunObject`, `RunRecipe` and `HandoffRequest`); `graph_status_test.cpp` and `desired_graph_test.cpp` use a few | Retiring the legacy adapter first ports those tests (S3c.6a), as a step of its own |
| `sendspin` is not in the harness | Still `notYetInHarness` (`test/nodes/all_types.hpp`) | Unchanged: it goes in S3c.6b |

### 0.2 Clusters: where several primitives work around one missing abstraction

**A. Readiness is written once per kind.** Engine companions and sink consumers each carry their own readiness
code (a deadline timer, a branch in `deadlinePassed`, a `waitingFor` kind, a failure code and text). This page adds
four more kinds (claims, stores, recordings, loops), an order between them (6.1's steps) and its own `ReadyEvents`.
Six kinds of "made in `configure`, ready or failed later, within a bound, in an order" are one missing abstraction:
the configuration's **readiness list**. NodeHost asks each item through one interface, one timer mechanism judges
every bound, `waitingFor` is read from the list, and the machine's one `resourcesReady` (built) fires when the list
is done. It is built first, behaviour-free, on the two kinds that exist (S3c.0); after that each primitive adds an
entry, not a mechanism.

**B. Cross-thread reports re-invent the generation.** 6.6's jobs "checked against the configuration's generation",
6.9's leaked record "hearing its late close", and section 5's handle that "lands only on the configuration that made
it" are each the configuration's `Channel` (built): one FIFO to the main loop per configuration, detached when it
ends. So `NodeHandle` is the `Channel`'s public face, every I/O completion and loop close is posted through it, and
the leaked record is the `Channel` re-pointed rather than detached. No primitive keeps a generation of its own.

**C. Ear-wide exclusives are found one at a time, or not at all.** `sendspin`'s `PortClaims` (TCP); an RTP
receiver's port, which is never found between ear's own nodes: rtp-source binds with `SO_REUSEADDR`, so two `rtp`
nodes on one port both bind and both reach `running`, Linux hands each unicast datagram to one socket only, and the
other plays silence with nothing said (`docs/engine.md#rtp-receivers`; the 5 s "never ran (UDP port held?)" path is
only for a holder without `SO_REUSEADDR`, such as another program); a recording path (never found: two `file` nodes
on one path corrupt one file); and a store file. They are one thing: something one configuration at a time may hold, judged before
anything is opened. 6.8's `ClaimRegistry` is that abstraction. What changes is where a claim comes from: a type's
explicit `cx.claim`, a store or recording request, and **each engine companion's shape**, so the framework never
names a shape's field to claim it (the module-run rule's pattern). A sound card's device may be one more [U: whether a
card node renamed in one plan opens its device before the old node closed it; the test card's file PCM is not
exclusive, so this is checked on the MOTU (`cards-and-clock.md` 4)]. It is designed below and built only if that check
shows the race.

Outside this slice, only noted: six private eventfd wrappers (`level_bell.cpp`, `keeper_thread.cpp`,
`descriptor_capture.cpp`, `engine_log.cpp`, `sources.cpp`, `sendspin/event_fd.hpp`). 3c deletes `sendspin`'s. The
others live in packages that may not depend on each other (`ear_output` depends on nothing of ear's), so one shared
wrapper would be a small utility package's job, not this slice's.

### 0.3 The interfaces as they must now be

Each is `ear_core`'s unless it says otherwise. Sections 5 to 6.11 keep every rule not restated here.

**The readiness list** (private to NodeHost, `src/core/readiness.hpp`; S3c.0, built:
`docs/engine.md#readiness-deadlines`). *Found building S3c.0:* an item fails with the lifecycle's `Fact` (NodeHost
feeds facts; the machine derives the code), not a `FailureReason::Code`; its `name` is its subject as `waitingFor`
names it, empty for a consumer (the wire names none, and two consumers are one wait); the list calls its owner's `Ask`
once for every item when its step is asked (unless it is already ready), the moment the owner starts it (S3c.3 to 5
open a recording, start a loop or make a companion there, once claims are granted), and keeps the bound's timer it
returns (null for none), so every kind keeps NodeHost's one rule for a timer that fires late; and `ReadyEvents` lands
with its first user (S3c.3's loops, S3c.4's stores), since today's two kinds tell ready through their own events.

```cpp
enum class ReadinessStep : unsigned { claims, opens, loops, companions }; // asked in this order
struct ReadinessItem {
	WaitingFor::Kind kind;
	std::string name;                               // empty where waitingFor names none (a consumer)
	ReadinessStep step;
	std::optional<std::chrono::milliseconds> bound; // from when it is asked; none: bounded elsewhere
	Fact fact;                                      // engine, consumerNeverBegan, as each kind fails today
	std::move_only_function<std::string()> overdue; // the failure detail, read when its bound passes
};
class ReadinessList { // one per configuration; main thread
public:
	using Ask = std::move_only_function<std::unique_ptr<Timer>(std::size_t index)>; // its bound's timer, or null
	std::size_t add(ReadinessItem item);                       // before begin()
	void begin(Ask ask);                                       // asks the earliest step not all ready
	void ready(std::size_t index);                             // cancels its bound; a step all ready asks the next
	std::optional<ReadinessOverdue> passed(std::size_t index); // its fact and detail unless ready; ends the list
	void disarm() noexcept;                                    // teardown: every bound cancelled
	bool isReady(std::size_t index) const noexcept;
	const ReadinessItem& item(std::size_t index) const;
	bool done() const noexcept;                                // every item ready: resourcesReady may follow
	std::vector<WaitingFor> waiting() const;                   // each item not ready, in item order, a repeat once
};
/// How a later-ready primitive tells it (an adapter's open, a loop's start, a claim's grant), with its first user.
/// Main thread, through the configuration's Channel; at most one of the two, never inside the call that asked.
class ReadyEvents {
public:
	virtual void ready() = 0;
	virtual void failed(std::string detail) = 0;

protected:
	~ReadyEvents() = default;
};
```

NodeHost adds the engine companions' items first, then the sinks' consumers', each in request order, and maps a
companion's wait to `block` while its block is not ready (it is made only once the block is).

`EngineCompanionEvents` stays as built (its `lost`, before ready or after, and its `faulted` are its own facts);
NodeHost's implementation of it feeds the item's `ready`. The list's `failed(index, detail)`, which ends it as a passed
bound does, comes with `ReadyEvents`. Companions also wait for the block, as built. Consumers are at the `companions`
step because a `file` consumer's `begin` writes a header into a recording opened at step 1; that is 6.1's
"4'".

**The node handle** (S3c.1, built: `docs/writing-a-node-type.md#the-node-handle`): section 5's `NodeHandle`, implemented by the configuration's `Channel`.
`NodeContext::handle()` and `NodeOf::handle()` return it, and `InsidesEvents` goes. `log(LogEntry)` sets `node` and
goes to the `LogSink` that `NodeHost::logTo` names (the one `main.cpp` gives captured output). `say`'s pacing is
`NoteGate` moved into core, per node, and `sendspin`'s gate is deleted in the same step, so nothing is paced twice.
*Found building S3c.1:* `NodeHost::subscribeLog` already existed (the framework's own `ear` entries), so the handle's
`log` goes to its sinks rather than a new `logTo`; each configuration copies them when it begins, and calls them on the
logging thread. The rate is judged on the saying thread (as `sendspin`'s was, so a held note is never queued), which
needs a clock any thread may read: `Timers::now()`'s contract widens to any thread, so core still reads no clock of its
own. The gate is the `Channel`'s (`src/core/note_gate.hpp`, private), so "per node, kept across a stop and a start,
reset by a restart" is "per configuration". The legacy `StartContext::events()` and `Ready::events()` return
`NodeHandle&` until S3c.6e. Section 5's "shared-owned, so it outlives every thread" holds as built, not further: the
configuration owns its `Channel` until teardown step 5 (a leaked one for ever), so a thread may call it until its
teardown ended, and a call after its detach is dropped. The harness gains `logs()`.

**Loops** (S3c.3): 6.3 as written, with three changes. `Loop::after` and `every` return `std::unique_ptr<Timer>` (core's
`timers.hpp` already defines a timer cancelled by its destruction), and that type replaces `LoopSource`.
`onWake(Wake&, ...)` watches `Wake::descriptor()` and calls `Wake::answer()` before the handler (the protocol
`source.hpp` states). `LoopAdapter::make` takes a `ReadyEvents&` (an item of step 2). Dedicated threads are named
`ear-<name> <node>`, as today's are.

**The I/O executor** (S3c.2): `class IoExecutor : public Executor` in core, and `IoThread` in `ear_runtime`. A job
holds `shared_ptr`s to what it fills, and its completion is a call taken from the configuration's `Channel`. So a
completion for a configuration that ended is dropped by the `Channel`, and a descriptor the job opened is closed by
the job's own destructor.

**Stores** (S3c.4): 6.7 as written, with `StoreAdapter::open` taking a `ReadyEvents&`.

**Claims** (S3c.5a): 6.8's `claim::TcpListen`, `claim::Udp`, `PortProbe` and registry, plus where implicit claims
come from:

```cpp
/// A claim's key: its kind and value ("tcp:8928", "udp:5004", "store:sendspin-ma.key", "recording:take1.wav").
struct Claim {
	enum class Kind { tcp, udp, store, recording, device } kind;
	std::string value;
};
/// The ear-wide exclusives an engine companion holds while it lives, by its shape: a rule beside moduleRunDelay
/// (companions.hpp), so NodeHost names no shape's field.
std::vector<Claim> claimsOf(const RtpReceiverShape& shape); // {udp, shape.port}
std::vector<Claim> claimsOf(const SoundCardShape& shape);   // none until 0.2 C's check shows the race; then the device
```

**Recordings** (S3c.5b): `CreateOf::recording(RecordingRequest) -> Recording&` replaces `openRecording`.

```cpp
/// A recording file this configuration writes, claimed at step 0 and opened at step 1 on the I/O executor. Valid from
/// configure until teardown step 5; descriptor() only from the sink consumer's begin() on (asserted in debug builds).
class Recording {
public:
	virtual int descriptor() const noexcept = 0;
	virtual const std::string& path() const noexcept = 0;

protected:
	~Recording() = default;
};
```

`RecordingAdapter::open(node, path, ReadyEvents&)` replaces today's synchronous `open`. A failure is the readiness
item's (`start-failed: cannot open <path>: <why>`, today's text), not `Configured::failed`.

**A sink's consumer as a loop state's member** (S3c.6b): 6.5's `consumedBy(loop, &State::member)` and
`consumedBy(loop)`. The consumer still runs on the sink's own thread: naming the loop only ties the consumer's life
to the state's (finished at step 3, before the state is destroyed at step 4). From S3c.6b `ToServer` dates each piece
from its own chunk's `startRawNs` (the capture anchor and its mailbox go: a chunk carries its cycle's start), still
with no upstream, as today; S3c.6d adds it, `FrameClock::enteredRawNs(chunk.startRawNs, offset,
Latency{0, chunk.upstreamFrames})`.
Whether a capture's chunks in a catch-up's burst also want `overdueNs` is still [U] (`timing-model.md` 4.5) and is
measured in S3c.6d.

**`sendspin`'s source and clock** (S3c.6c): `timing-model.md` section 6 as designed; the interfaces are built.
`SendspinClock : SourceClock` holds today's `SendspinCode` playback state and its `Mailbox<PlayProgress>`. Its test
seams replace the feed's (`dryTicks()`, `dryFrames()`, `lastHeld()`, `lastSkipped()`, `clamped()`), so
`CycleJudge::librarySettle`'s `dry` cause reads the clock, not a feed. The companion's `shortfall` counter is the
operator's view; the judge does not read it.

**`TypeSpec`** (S3c.3 and S3c.4): 6.11's members `stores` and `loops` after `banks`, each defaulting to an empty list.
Today's `TypeSpec<Settings, BankList>` keeps its `requires` clause. U1 still decides the spelling.

### 0.4 Obsolete in this page

| Where | What | Why |
|---|---|---|
| 2 | `LibraryView`, the debt and `skipThenRead` among the seams to clear | Deleted by T2 |
| 9 | "The feed's write position" as a log line's stamp | No feed after S3c.6c: the worker counts the frames it has written to the source, and its log sink stamps each line with that count |
| 6.1 | The readiness table as four steps of their own, and `ReadyEvents` beside the companions' events | 0.2 A, 0.3 |
| 6.2 | `EventWake` in `ear_runtime`, moved from `ear_pipewire` | 0.1: core's `Wake` is enough |
| 6.6 | Jobs checking the generation | 0.2 B |
| 6.8 | `udp:<p>` claimed "by every RTP receiver companion" as a rule of NodeHost's | `claimsOf(shape)` |
| 6.10 | `int fd = cx.recording(...)` | `Recording&` |
| 8, visible change 5 | A stopped `sendspin` node closing its connection | Q-3c1 (0.6): kept open unless the owner says otherwise |
| 12 | `sendspin_code.*`'s playback half still to be written | Written (T2); moved whole |
| 13 | The cut | 0.5 |

### 0.5 The build steps

Each is one reviewable commit, green on every test and `make format-check`, and changes the library pages it makes
stale. **Revert proof**: with the step's production change reverted and its tests kept, the named tests fail; each
step's commit message records the run. A step that changes no behaviour has no revert proof, but it must pass every
existing test unedited: a test that needs editing means behaviour moved, and the step is wrong.

| Step | What | Behaviour | Proof |
|---|---|---|---|
| **S3c.0 Readiness list** (done: dffa92077a) | `readiness.hpp`; NodeHost's engine-companion and sink-consumer readiness, their deadlines and `waitingFor` read from it; the `Deadline` enum and its branches go | None | Every `node_host_test.cpp` readiness and `waitingFor` case passes unedited (each kind's deadline and text, a ready first wins, a restart's own, a consumer's 5 s). New unit rows on the list: steps gate in order, a bound runs from its asking, a failure ends the list, `waitingFor` in item order. Mutation check: asking step 3 before step 1 is ready fails a new row. **Owed by the first step that adds an item
outside the `companions` step (S3c.3, S3c.4 or S3c.5a):** a `node_host_test.cpp` row, with its revert proof, showing a
companion's deadline does not start until the earlier step is ready (S3c.0's rows prove that on the list alone) |
| **S3c.1 Node handle** (done: 26d2bf56cf) | `node_handle.hpp`; the `Channel` implements it; `NodeHost::logTo`; pacing in core; `InsidesEvents` and `sendspin`'s `NoteGate` go (`notes_test.cpp`'s gate cases move to core) | None an operator sees: the same notes; `notes-dropped` said by the framework | Section 5's tests: a log carries its node; after detach, dropped; an old configuration's late `say` dropped; a goodbye heard; 32 then one a second per node, kept across a stop and a start, reset by a restart; `lost` never held. Revert proof: core without the gate fails the pacing rows |
| **S3c.2 `ear_runtime`, the I/O thread** | The package; `IoThread`; `ResourceAdapters` (one member per kind; a null adapter refuses its kind); `ear_refuse_node_type_links` refuses `ear_runtime` and `ear_pipewire`; `main.cpp` stops the thread after `stopAll` and the keeper | None | A completion after its configuration ended is dropped and its descriptor closed; a job busy at stop (2 s, the log record, `std::_Exit`); a null adapter's refusal; the link refusal per package (configure fails on a test type that links either). Revert proof: the link refusal's test fails without it |
| **S3c.3 Loops** | 6.3 with 0.3's changes; `Loops`, `TypeSpec::loops`; `StrandPool`, `DedicatedLoops`; staging; `closeToOthers`; teardown step 4 closes loops (beside the run object's `close`, which stays until S3c.6e); the harness's loops and fake clock | None for shipped types | Section 13's S3c.3 list, through a test type in `ear_core_tests` and on real threads in `ear_runtime`'s tests; `ear.loop-slow` and `ear.loop-source-left` in `output-records.json`. Revert proofs: with staging off, "staged work runs only once configured" fails; with the source count off, "a source left armed" fails |
| **S3c.4 Stores** | 6.7; `ConfinedStores` and the boot sweep over declared stores, in `ear_runtime`; store items at step 1 | None for shipped types (`sendspin` keeps its key store until S3c.6b) | Section 13's S3c.4 list; U1's compile test of every `TypeSpec` subset. Revert proof: with narrowing before judging off, "a refused file is narrowed before it is read" fails |
| **S3c.5a Claims** | `Claim`, `ClaimRegistry` at step 0, `KernelPortProbe`; `cx.claim`; `claimsOf(RtpReceiverShape)`; `ear.claim-wait`; `waitingFor` `claim` (wire, fixtures and `protocol.ts` together) | Visible change 3: an `rtp` node on a port another program holds fails at once (`Address already in use`, not the 5 s deadline); two `rtp` nodes on one port, where today both run and one is silently starved, refuse the second `held by node <n>` | Section 13's claim cases; a system test for visible change 3; a removed `rtp` and an added one on its port in one plan (the new one waits, then runs). Revert proof: without `claimsOf`, the two-`rtp` system test finds both nodes `running` and one of them silent (today's behaviour), instead of the second refused `held by node` |
| **S3c.5b Recordings** | `Recording`, `RecordingAdapter::open` on the I/O executor, the implicit `recording:` claim; `file` on it; `openRecording` goes | Visible change 2: a second `file` on one path is refused; a remove and an add on one path works | `file_test.cpp` on the harness's recording fake; a system test for visible change 2; an open past its 5 s bound. Revert proof: without the claim, the two-`file` system test finds both running |
| **S3c.5c The leaked record** | The `Channel` re-pointed at the leaked record; port claims abandoned, store and recording claims kept; `ear.close-late` and the restart of the nodes it refused (R15); `ear.close-overdue` gains its counts | Only after a leak, which no shipped type makes in tests | Section 13's leaked-record cases, with a test type whose loop never drains. Revert proof: detaching instead of re-pointing fails "the late close starts the refused node" |
| **S3c.6a Core's tests off the legacy SPI** | `node_host_test.cpp`, `graph_status_test.cpp`, `desired_graph_test.cpp`: their legacy test types rewritten as classes on `makeNodeType`, a handoff case as a sink | None (tests only) | Each ported case asserts what it asserted. Only the legacy-only cases (a recipe, a run object's close, the two-phase start) stay on the legacy SPI, until S3c.6e deletes them with it |
| **S3c.6b `sendspin` on the class** | `sendspin_spec.hpp` and `Sendspin`; the worker a dedicated loop's state, the notifier a loop; the key file a store; the listening port a claim; `to` passed to `to_server`, a sink consumed by `ToServer` (still dated by the cycle's start); the feed and today's code kept as the type's own spec code (`from` played from the feed); `NodeFolders` and `allNodeTypes`' argument go; `notYetInHarness` goes; the thread guard | Visible change 1 (a corrupt key file fails before `running`); otherwise as today, subject to Q-3c1 | `type_test.cpp` (section 12); `sendspin` in the all-type harness test; every `sendspin` system test with its lines unchanged (U4); `clock_test.cpp` and `code_test.cpp` unedited; the guard per spelling. Revert proof: the corrupt-key system test sees `running` first |
| **S3c.6c The source and its clock** | `from_server` a source with `SendspinClock` (T2's wrapper, moved), its clock wake watched on the notifier loop; `feed.*` and `sendspin_code.*` go; the node's code is `passThrough` | None in timing: `timing-model.md` 6.4's tests pass unedited on the source | T2's tests on the source (`clock_test.cpp`, through the real engine); the clock's unit rows through `TypeHarness<spec, Sendspin>` with scripted facts; `requiredLeadMs` from the source's frames. No revert proof (a move); T2's clamp-idle row is its mutation check |
| **S3c.6d Capture dated by its upstream** | `ToServer` dates by `enteredRawNs` with the chunk's `upstreamFrames`; `docs/node-types.md#clock` loses its "not counted yet" | Yes: a card's capture reaches the server dated earlier by the card's capture latency | A unit row: a chunk with `upstreamFrames` 480 dates its first frame 10 ms before its cycle's start at 48 kHz; `sendspin_streams_test.cpp`'s steps unchanged; whether `overdueNs` should date burst chunks measured and recorded [U]. Revert proof: the unit row fails |
| **S3c.6e The legacy adapter retired** | `LegacyInstance`, `StartContext`, `StartResult`, `RunRecipe`, `Ready`, `RunObject`, `HandoffRequest`, `RtWriter`, `HandoffConsumer`, `HandoffAdapter`, `ThreadHandoffs`, `NodeTypeOf`, `NodeConfig::start`; `handoff_test.cpp`; `docs/writing-a-node-type.md#the-old-interface` | None | The tree builds without them and every test passes; no name of them is left under `src/` or `test/` |
| **S3c.7 The log patch** | Section 9 as written, with 0.4's stamp; `kLogReach`, `kLogLag` and `inLoss`'s lag go; the system tests' `Lost sync` matching by size and their `stepsInLoss` bound become matching by place (`docs/testing.md#the-librarys-own-word`) | The library's lines become `log` records naming their node | Section 13's S3c.7 list; U2's count of unattributed lines. Revert proof: without the patch, the two-node attribution test fails |
| **S3c.8 Words** | This page and `timing-model.md`'s S3c rows retired; what lasts moved (sections 5 to 10 into `docs/writing-a-node-type.md` and `docs/architecture.md`) | — | The anchor check over every page touched |

S3c.5a to 5c may land in any order after S3c.4, and S3c.6a at any time before S3c.6e. S3c.6b needs S3c.0 to 5a (no
recording and no leaked record). S3c.7
needs only S3c.6b (the worker's state sets the sink). T5 follows S3c.6c with its first user (`timing-model.md` 7).

### 0.6 Questions for the owner

**Q-3c1. When you stop a `sendspin` node, should it stay connected to Music Assistant, silent, or disconnect?**

*Scenario.* The kitchen is a `sendspin` node that Music Assistant reaches by dialling ear. You press Stop on it in the
editor. *Stay connected* (today's behaviour): Music Assistant still lists the kitchen player and keeps it in its
group; the kitchen plays nothing, and pressing Start plays again at once. *Disconnect*: Music Assistant shows the
kitchen as gone and takes it out of the group. Pressing Start makes ear ready again, but in tests a server that
dialled by hand did not dial back within 30 s (`docs/node-types.md#stop-restart-disable`), so the kitchen may stay
silent until Music Assistant finds it again.

*Recommendation: stay connected.* It is what a stop does today, it matches "a stopped node keeps everything" (R26),
and it avoids the gap before the server dials back. Restart, disable and removal still say goodbye. (Section 8's
visible change 5 is the other answer.)

**Q-3c2. Should two `rtp` nodes be allowed on the same UDP port when they listen to different multicast groups?**

*Scenario.* Your FPP sends two shows as multicast, one to 239.0.0.1 and one to 239.0.0.2, both on port 5004, and you
add `rtp showA` and `rtp showB`. *Refuse*: the second fails "cannot bind UDP port 5004: held by node showA", and you
give the shows different ports in FPP. *Allow*: both play; PipeWire binds each to its group, but ear has no multicast
test yet, so this would be the first.

*Recommendation: refuse for now*, and allow it (a claim keyed by group and port) once a multicast test exists.

### 0.7 Open items, re-checked

U1 to U8 (section 18) stand; U4 also covers Q-3c1's answer in the `sendspin` system tests. New: the card device race
(0.2 C) and `overdueNs` for chunks in a burst (S3c.6d).

## 1. The owner's words that bind this slice

The R and G numbers are the register the node framework's design used.

| # | The owner's words | Date | What it means here |
|---|---|---|---|
| R1 | "what are the core primitives that ear should host so that new nodes can be added simply and only require defining the truly custom bits that its node type demands" | 2026-11-07 | The primitives `sendspin` wrote for itself become the framework's; a type writes its protocol |
| R9 | "Nodes should not have direct access to stdout or stderr. The core framework should expose the necessary primitives for events, diagnostics, and logging. These should be real-time safe for any audio loops that need them, and there can be non real-time bases for the rest of the system." | 2026-11-07 | A node's threads log through its handle; the library's own lines are attributed to the node (section 9) |
| Q5 | "Strands, dedicated on request" | 2026-11-07 | Loops (6.3) |
| R13 | "mailboxes could be supportable in either mode. But why must mailboxes be built before the run object exists? Is this a side effect of the naive node state machine model I described?" | 2026-11-08 | The instance or a loop's state owns what it shares with the code; no kept values |
| R14 | "G4 is asking the wrong question. The node audio threads should be concerned with reading inputs and writing outputs. The framework should provide the necessary primitives to connect inputs/outputs across nodes AND anytime a node needs to cross inputs/outputs to non-realtime code (eg pipe outputs through a mailbox?)" | 2026-11-08 | A type's threads meet audio only as a source's writer or a sink's consumer (built: `docs/architecture.md#companions`) |
| R15 (G3) | "(a)": a store, recording path or port held by a leaked configuration is refused with a detail saying the node starts when that configuration closes; the late close starts it | 2026-11-08 | 6.8, 6.9 |
| R16 | "I like the basic shape of the state machine proposal. And most importantly: it should be per-node. Some node stopping/reconfiguring/failing does not effect the graph globally" | 2026-11-08 | Every primitive is per node except the I/O executor and the strand pool, which are shared and bounded |
| R19 | "Shouldn't that all be part of the build stage? How is build different from [re]configure?" | 2026-11-08 | One `configure` acquires and constructs; each primitive is asked for, or reached by handle, inside it, and readied by the framework |
| R20, R21 | "A “run” can’t fail — only a node can fail. We should drop the “run” concept entirely and work with “graph” and “node” concepts. The graph is always running, while nodes can be added to it, configured, wired, start/stop all synchronously within the graph runtime"; "I meant “asynchronously”" | 2026-11-08 | The handle is the node's; a primitive lives as long as the node's configuration (an internal word); a closing thread's late word is dropped by a generation no type sees |
| R23 | Exactly three hooks: `configure(old, new)`, `start()`, `stop()` | 2026-11-08 | No close hook: a loop's state is destroyed on its own loop, after the goodbye `stop()` posted |
| R24 | "too technical for me to properly answer" (on where reconfigure lives and how it reaches a type's threads) | 2026-11-08 | Hooks reach loops only by typed posts (D-A4); this page's questions are the designer's (section 17) |
| R26, R27 | "Keeps everything" (what a stopped node keeps); "Both words" (`stopped` and `disabled`) | 2026-11-08 | A `stopped` node keeps every primitive; a `disabled` one releases every one by the full teardown (built semantics, `docs/architecture.md`) |
| R33 | "Two versions" (of `configure`, each offering only its legal calls) | 2026-11-08 | Each primitive's call sits in the context of the mode that may make it (6.1) |
| G1 | "(a)": a new `ear_runtime` package for loops, wakes, the I/O executor, the probe and stores | 2026-11-08 | Section 11 |
| G14 | "(a)": loops declared in the spec as handles | 2026-11-08 | 6.3 (each handle names its state type) |
| G15 | "(a)": resources reached by handle, not through a recipe lambda | 2026-11-08 | Inside `configure` (R19) |
| G17 | "(a)": the handle reached from the framework's context | 2026-11-08 | `node.handle()` on `NodeOf` |

## 2. What slices 3a and 3b leave

| Seam | Where, as built (or as 3b leaves it) | Cleared by |
|---|---|---|
| `sendspin` derives from `NodeTypeOf`, writes its own `NodeConfig` and runs behind the legacy adapter (`LegacyInstance`, `StartContext`, `StartResult`, the recipe, `RunObject`, `HandoffRequest`, `ThreadHandoffs`) | `sendspin_type.cpp`, `legacy_instance.hpp` | S3c.6: the class, in one step; the old SPI goes |
| `NodeFolders` and `makeSendspinNodeType(const NodeFolders&)`; the boot sweep and its boot log | `node_folders.hpp`, `sendspin_type.cpp` | stores (6.7) |
| A node's way out is `InsidesEvents`, reached as `NodeOf::handle()` | `node_type.hpp`, `node_host.cpp` | the node handle (5) |
| Claims live in sendspin's type, across its nodes only (`PortClaims`, `HeldPort`, `probeListenPort`); `rtp` claims nothing, nothing claims a recording path | `port_claims.*` | claims, ear-wide (6.8) |
| A leaked configuration keeps its port or file while the next one starts | `NodeHost`'s close deadline | the leaked-configuration record (6.9) |
| `file`'s recording opened synchronously on the main loop by `cx.openRecording` | `lifecycle.hpp`, `file.cpp` | recordings, deferred (6.10) |
| `notYetInHarness = {"sendspin"}` | `test/nodes/all_types.hpp` | section 10 |
| sendspin's threads: the worker (a 10 ms `poll()`), the notifier, their stop `EventFd` | `worker.cpp` | loops (6.3) |
| sendspin's feed, mailboxes, bell, capture anchor, `SendspinCode` and handoff | `feed.cpp`, `mailbox.hpp`, `sendspin_code.cpp` | a source and a sink companion (`docs/writing-a-node-type.md#sources`, `#sinks`), when S3c.6 moves it |
| Its key file: named, 0600, read, trusted, created, swept at boot | `key_store.cpp` | stores (6.7) |
| Note pacing (32, then one a second) | `note_gate.cpp` | the node handle (5) |
| The library's log lines are unattributed `stderr` records | sendspin-cpp `src/platform/logging.h` | the log patch (9) |

Three facts shape what is left. **The instance exists before the code drives** (built), so nothing a type shares has to
be made before it. **`ear_core` may not touch threads, files or sockets**, so loops, stores, the probe and the I/O
thread are interfaces in core and adapters in `ear_runtime` (G1). **A node's configuration outlives a stop and a start,
and from 3d an in-place change**: a primitive bound to it (a loop and its state, a store, the handle) must be right for
every settings value the node will take; a primitive bound to a setting (a claim on a port, a recording on a path) must
be replaceable make-before-break (3d).

## 3. The design in one paragraph

A type's spec declares its **stores** and **loops** as compile-time handles beside its banks, each loop with the type
of its **state**. Its creation `configure` asks for **claims** and **recordings** (and companions), takes store handles,
and gives each loop its state. The framework judges claims first, then opens stores and recordings on its **I/O
executor**, then starts loops; until then everything posted or armed from `configure` is staged, and it runs, in order,
once the node is configured. `start()` and `stop()` post to the node's loops; a hook reaches a loop's state only by a
post typed by the loop's handle. A node keeps its loops, their states, its stores and its **node handle** through every
stop, start and (from 3d) in-place change. A leaked configuration keeps listening, and its late close starts the nodes
its claims refused (R15). Implementations live in `ear_runtime`. `sendspin` moves onto `TypeSpec` and `TypeHarness`, its
worker a loop with a state.

## 4. Journeys: what a type author writes

### 4.1 A type with its own threads: sendspin

**Before** (abridged, [V]): the type holds a claims map across its nodes; `start` claims and probes, builds the whole
run object (which reads the key file and starts two threads), acquires the handoff, declares the feed's latency and
returns the code with a recipe that only hands the object over (`sendspin_type.cpp`).

**After** (slice 3c). The spec declares the banks, the key file and the two loops, the worker with its state type.
Creation's `configure` acquires and constructs: it gives the worker its state and names the sink's consumer; `start()`
and `stop()` post to the worker (`stop()` once after every `start()`, as built). The change version is 3d's
(`reconfigure.md` 6). No line carries a resource from one hook to another, and no hook touches the worker's state but by
a post.

```cpp
// src/nodes/sendspin/sendspin_spec.hpp
namespace ear::nodes::sendspin {
namespace rows {   // today's rows (settings.hpp); 3d marks them .inPlace()
inline constexpr auto server = setting::optional("server", setting::text<ServerUrl::parse>("ws://<host>[:<port>]/<path>"));
inline constexpr auto port = setting::defaulted("port", setting::number(1, 65535), 8928).onlyWithout(server);
inline constexpr auto name = setting::optional("name", setting::text<parseName>("1 to 64 bytes of UTF-8 ..."));
}
inline constexpr auto to = bank::input<"to">;
inline constexpr auto from = bank::output<"from">;
inline constexpr auto toServer = bank::insideOutput<"to_server">;     // its sink companion joins here
inline constexpr auto fromServer = bank::insideInput<"from_server">;  // its source companion joins here
/// sendspin-<node>.key in the graph file folder: mode 0600, at most 64 KiB, created empty, trusted only as a key record.
inline constexpr auto keyFile =
	store::file<"key">({.mode = 0600, .maxBytes = 64 * 1024, .initial = &emptyKeyRecord, .distrust = &whyNotKeyRecord});
class Worker;   // worker.hpp: the library's client, pairing, the time filter, mDNS; lives on the worker loop
inline constexpr auto worker = loop::dedicated<"sendspin", Worker>;   // the library's main loop
inline constexpr auto notifier = loop::dedicated<"notify">;           // no state: the worker arms it
inline constexpr TypeSpec spec{.settings = SettingsTable<rows::server, rows::port, rows::name>{"sendspin"},
                               .banks = Banks<to, from, toServer, fromServer>{},
                               .stores = Stores<keyFile>{}, .loops = Loops<worker, notifier>{}};
using Checked = CheckedOf<spec>;

class Sendspin {   // the node's instance: main-loop state only
public:
	static Members<spec> ports(const Checked&) {
		return {to(members("left", "right")), from(members("left", "right")),
		        toServer(members("left", "right")), fromServer(members("left", "right"))};
	}
	explicit Sendspin(NodeOf<spec>& node) : node_(node) {}
	Configured configure(CreateOf<spec>& cx, const Checked& now);
	void start() { node_.post(worker, [](Worker& w) { w.begin(); }); }   // make the client, start it, listen or dial
	void stop() { node_.post(worker, [](Worker& w) { w.end(); }); }      // goodbye; the client stopped and destroyed
private:
	NodeOf<spec>& node_;
};
}

// src/nodes/sendspin/sendspin.cpp
Configured Sendspin::configure(CreateOf<spec>& cx, const Checked& now) {
	if (!now.get<rows::server>()) cx.claim(claim::TcpListen{now.get<rows::port>()});   // judged first (6.8)
	auto clock = std::make_unique<SendspinClock>(cx.format().rate);   // timing-model.md 6: honest, local skip
	SendspinClock& progress = *clock;                                    // freed with the source, after the worker
	SourceWriter& played = cx.source(fromServer, {.frames = feedFramesFor(cx.format())}, std::move(clock));
	cx.loop(worker, std::make_unique<Worker>(WorkerParts{
		.handle = node_.handle(), .keys = cx.store(keyFile), .played = played, .progress = progress,
		.self = cx.executor(worker), .notifier = cx.executor(notifier), .settings = settingsFrom(cx.node(), now)}));
	const auto half = static_cast<std::uint32_t>(FrameClock(cx.format().rate).frames(500ms));   // no rate arithmetic
	cx.sink(toServer, {.frames = half}).consumedBy(worker, &Worker::toServer);   // begun before running
	cx.use(passThrough({bankPair(to, toServer), bankPair(fromServer, from)}));
	return Configured::applied();
}
std::unique_ptr<const NodeType> makeSendspinNodeType() { return makeNodeType<sendspin::spec, sendspin::Sendspin>(); }
```

The worker's state takes what it uses in its constructor (on the main loop, inside `configure`) and arms nothing there:
the store is not open yet, and the loop not started. Its work runs on the worker loop once the node is configured;
`begin()` reads the key record, makes the client and arms the tick; `end()` is the goodbye. Its destructor runs on the
worker loop when the configuration is torn down (6.9, step 4) and destroys whatever is left:

```cpp
void Worker::begin() {   // on the worker loop, after start()
	KeyRecord record = KeyRecord::parseTrusted(keys_.text());   // the store's rule already trusted the text
	startClient(std::move(record));                             // make, start, listen or dial
	tick_ = self_.every(std::chrono::milliseconds{10}, [this] { tick(); });            // client.loop(), watch, mDNS
	notifier_.post([this] { notified_ = notifier_.onWake(played_.clockWake(), [this] { notify(); }); });
}
void Worker::end() {     // on the worker loop, after stop(); also queued ahead of the destructor at removal
	notifier_.post([this] {
		notified_.reset();                  // on its own loop: no notify runs after this line
		self_.post([this] { tick_.reset(); stopClient(); });   // client->stop(): goodbye, its threads joined
	});
}
// The library's sync thread writes the source companion, as it writes the feed today:
std::size_t Worker::on_audio_write(std::uint8_t* data, std::size_t length, std::uint32_t timeoutMs) {
	if (!streamAsked_.load(std::memory_order_acquire)) {
		played_.idle(std::chrono::milliseconds{timeoutMs});   // sleeps its patience, before or after the close
		return 0;
	}
	/* as today: convert, then played_.write(scratch_, frames, std::chrono::milliseconds{timeoutMs}) */
}
```

What the author no longer writes: a thread, a poll loop, an eventfd, a feed, a capture anchor, realtime code beyond its
source's clock (`timing-model.md` 6.1: a skip and a report, the report in its own mailbox),
note pacing, a claims map, a probe, a `HeldPort`, a key file's open, mode or sweep, a `NodeConfig`, a `NodeTypeOf`, a
latency declaration, a folder parameter, a close hook, or a lambda that carries resources between hooks.

A stopped `sendspin` node then keeps its configuration but says goodbye in `stop()` (its worker's `end()`), so its
connection closes (today, on the legacy adapter, a stopped node keeps its connection and plays the feed's leftover, up
to about 43 ms, after a start; the source companion clears its ring at a stop).

### 4.2 A type that needs a store, a claim and a consumer

The example is an RTP sender (not asked for; it illustrates the interface): it sends its input bank as L24 to a unicast
address from a fixed source port, which no two nodes may share (a claim), and keeps its SSRC across restarts so a
receiver does not see a new stream each time (a store).

```cpp
namespace ear::nodes::rtpsend {
std::string newSsrc();                                              // eight random hex digits
std::optional<std::string> whyNotSsrc(std::string_view text);      // "malformed: ..." unless eight hex digits
inline constexpr auto send = bank::input<"send">;
inline constexpr auto wire = bank::insideOutput<"wire">;            // its sink companion joins here
inline constexpr auto ssrcFile =
	store::file<"ssrc">({.mode = 0644, .maxBytes = 64, .initial = &newSsrc, .distrust = &whyNotSsrc});
class Sender;
inline constexpr auto control = loop::strand<"control", Sender>;
inline constexpr TypeSpec spec{.settings = SettingsTable<rows::address, rows::port, rows::sourcePort, rows::channels>{"rtpsend"},
                               .banks = Banks<send, wire>{}, .stores = Stores<ssrcFile>{}, .loops = Loops<control>{}};

/// The control strand's state, and the sink's consumer.
class Sender final : public SinkConsumer {
public:
	Sender(NodeHandle& handle, StoreHandle& ssrc, Loop& control, Ipv4 address, std::uint16_t port, std::uint16_t sourcePort);
	void begin() override;                     // reads the SSRC; socket, bind sourcePort, connect; failure: lost(...)
	void consume(const Chunk& chunk) override; // RTP timestamp from chunk.position; RTCP every 5 s of audio
	void finish() override;                    // BYE, close the socket
};

class RtpSend {
public:
	static Members<spec> ports(const CheckedOf<spec>& s);
	explicit RtpSend(NodeOf<spec>& node) : node_(node) {}
	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>& now) {
		cx.claim(claim::Udp{now.get<rows::sourcePort>()});
		SinkRequest& sink = cx.sink(wire, {.frames = static_cast<std::uint32_t>(FrameClock(cx.format().rate).frames(100ms))});
		cx.loop(control, std::make_unique<Sender>(node_.handle(), cx.store(ssrcFile), cx.executor(control), ...));
		sink.consumedBy(control);                              // the loop's state is itself the consumer
		cx.use(passThrough({bankPair(send, wire)}));
		return Configured::applied();
	}
private:
	NodeOf<spec>& node_;
};
}
```

Neither `start()` nor `stop()` is written: the drives that bind and unbind the sink start and stop the sending, and the socket
stays open while stopped. The SSRC file is `rtpsend-<node>.ssrc`, created with `newSsrc()` on first configure; a
corrupt one fails the node `start-failed: rtpsend-<node>.ssrc: malformed: ...` and is not replaced. A second node on the
same source port fails `start-failed: cannot bind UDP port 5006: held by node <n>`; removing one node and adding
another on that port in one plan works, the new one starting once the old one let go, with `node <new>
ear.claim-wait: udp:5006 held by node <old>, which is closing` while it waits. Its in-place change of address is
`reconfigure.md` 6's example.

## 5. The node handle

`NodeHandle` replaces `InsidesEvents` (`node_type.hpp`) as what `NodeOf::handle()` returns. Slice 5 builds over it
(`Report<spec>`, `control-plane.md`). The generation it carries is internal: records and the wire never show it.

```cpp
// ear/core/node_handle.hpp: replaces InsidesEvents
/// A node's way out, from any of its threads; never blocks. Shared-owned by the framework, so it outlives every thread
/// that can reach it, a leaked configuration's included. What it carries lands only on the configuration that made it.
/// A stop, a start and (3d) an in-place change keep it; a restart makes a new one.
class NodeHandle {
public:
	virtual const NodeName& node() const noexcept = 0;
	/// A fact: the node cannot go on. It fails `lost` with `detail` once it has rested (`running` or `stopped`), and
	/// `start-failed` before that (as built). Ignored once teardown began.
	virtual void lost(std::string detail) = 0;
	/// Free text for people. The framework sets `node`; an empty `source` is the type's name. Straight to the log sink
	/// (any thread, never blocks), paced per node by the encoder as built. Dropped once the handle is detached.
	virtual void log(LogEntry entry) = 0;
	void debug(std::string text);  void info(std::string text);  void warn(std::string text);  void error(std::string text);
	/// Interim, until slice 5 splits notes into status and events: `node <n> says <text>`, on the main loop in order.
	/// Paced per node: 32 at once, then one a second; the next note that passes is preceded by `notes-dropped <n>`. A
	/// stop or a start does not reset it; a restart does. Heard until the handle is detached, so a goodbye is heard.
	virtual void say(std::string text) = 0;
protected:
	~NodeHandle() = default;
};
```

- **Order.** `lost` and `say` cross to the main loop in order (as `InsidesEvents`' channel does), so a note is ordered
  with node states. `log` goes straight to the sink, unordered with them, as captured output is. Its detach check is an
  atomic flag.
- **Pacing.** `say`'s pacing is sendspin's `NoteGate` moved into core, one gate per node; `lost` is never held back.
- **The sink.** `NodeHost::logTo(LogSink&)`, wired in `main.cpp` to `StatusLog`, the sink captured output already uses.
- **A leaked configuration's handle** is detached from the node and re-pointed at the leaked-configuration record
  (6.9), which hears its late close; `lost`, `log` and `say` from it are dropped.

## 6. The primitives

### 6.1 Each primitive through the lifecycle, and the contexts' members

A `disabled` node has no configuration, so every primitive is as after the teardown (claims released, stores and
recordings closed, loops' states destroyed, companions gone); enabling it starts again from empty. A `stopped` node keeps
them all (R26).

| Primitive | `configure` (creation) | readiness (step) | `configured` | `running` (`start()`) and `stopped` (`stop()`) | teardown (step) |
|---|---|---|---|---|---|
| Claim (6.8) | `cx.claim(...)`; implicit for stores, recordings, RTP receivers | judged first (1), then ports probed | held | held | released last (5); abandoned or kept by a leak |
| Store (6.7) | declared in the spec; `cx.store(h)`, a handle | opened on the I/O executor (2), 5 s | `text()`, `replace`, `replaceLater` on the node's loops | the same | usable until 4; freed (5) |
| Recording (6.10) | `cx.recording({path})`: a descriptor, usable once configured | claimed (1), opened (2), 5 s | its consumer's `begin` writes the header | written by its consumer while driven | closed (5) |
| Loop (6.3) | declared in the spec; `cx.loop(h, state)`, `cx.executor(h)`; posts and arming staged | started (3) with its state, nothing run | staged items released, in order | `start()` and `stop()` post to it | closed to others (1); runs what was posted, then destroys its state on its thread (4); freed (5) |
| Node handle (5) | `node.handle()` | — | `lost`, `log`, `say` | the same | detached at 1 for facts; re-pointed at a leak |
| Source writer (`docs/writing-a-node-type.md#sources`) | `cx.source(bank, ...)` | the companion's | written by one thread | written while the type plays | closed (1); ring freed (5) |
| Sink consumer (built, `sink.hpp`) | `cx.sink(bank, ...)`, `consumedBy(...)` | `begin` (4') | — | `consume` on the sink's thread for exactly the cycles the code drives | drained, `finish` (3) |
| I/O executor (6.6) | — | runs the opens (2) | runs store writes | the same | completions after teardown dropped |

This slice's members of `CreateOf<spec>` and `NodeOf<spec>` (each only where its mode may call it, so a call in the
wrong mode does not compile, R33; the `ChangeOf<spec>` members are 3d's, `reconfigure.md`):

| Context | Member | Rules |
|---|---|---|
| `CreateOf<spec>` | `claim(claim::TcpListen)`, `claim(claim::Udp)` | judged at readiness step 1 (6.8) |
| `CreateOf<spec>` | `recording(RecordingRequest)` → `int` | claims `recording:<path>`; opened at step 2 (6.10); replaces 3a's `openRecording` |
| `CreateOf<spec>` | `store(handle)` → `StoreHandle&` | a handle of `Spec.stores` (static); open and trusted once configured (6.7) |
| `CreateOf<spec>` | `loop(handle, state)`, `executor(handle)` → `Loop&` | a handle of `Spec.loops`; the state's type is the handle's; staged until configured (6.3) |
| `CreateOf<spec>` | `post(handle, work)` | staged until configured (6.3) |
| `SinkRequest` | `consumedBy(loop, &State::member)`, `consumedBy(loop)` (a state that is itself a `SinkConsumer`) | beside 3a's `consumedBy(std::unique_ptr<SinkConsumer>)` |
| `NodeOf<spec>` | `handle()` → `NodeHandle&`, `post(handle, work)` | the node handle (5); posts to a loop's state (6.3) |

- **Stores and loops are declared, not discovered**: every node of the type has them, the boot sweep knows every store
  before any node exists, and slice 5's `writer::loop` and action calls take a loop handle the spec lists. A loop
  declared with a state type must be given its state by creation's `configure`.
- **Rules a compiler cannot check** (a sink with no consumer, a consumer named twice, a loop with a state type not
  given one) fail the node `start-failed` with the rule's text at `configure`'s return, as every built rule does (the
  built contexts record the broken rule on the node and fail it, even when the type catches the throw). Store and loop
  handles the spec lacks, and a post whose function takes another loop's state type, do not compile.
- Kinds that become ready later tell the framework through one small interface, main thread only:

  ```cpp
  class ReadyEvents {
  public:
  	virtual void ready() = 0;                    // at most once, never inside the adapter's make or open
  	virtual void failed(std::string detail) = 0; // at most once, before ready: the node fails start-failed, `detail`
  protected:
  	~ReadyEvents() = default;
  };
  ```

**Readiness**, ordered by cost and side effects so that a refusal costs nothing:

| # | What | When it fails |
|---|---|---|
| 1 | **Claims**, in the same main-loop turn as `configure` returned (6.8). A refusal fails the node now, and nothing below is touched. A wait holds every later step. Once all are granted, ports are probed (main thread, a non-blocking bind) | `start-failed: <claim's text>` |
| 2 | **Stores and recordings**, opened on the I/O executor, one job each, in request order (5 s each) | `start-failed: <file>: <why>`; `start-failed: cannot open <path>: <why>` |
| 3 | **Loops**, once step 2 is ready: dedicated threads started, strands registered, each with its state. Threads last, so a refused store starts none | `start-failed: cannot start loop <name>: <errno text>` |
| 3' | Companions, once the block is ready and step 1 granted, in parallel with 2 and 3 (built: `docs/engine.md#engine-companions`, `#readiness-deadlines`; a sink once its consumer began, a source at once) | `engine-refused: <detail>` |
| 4' | Every sink consumer has begun (built) | `start-failed: <detail>` |

Then the node is `configured`: the code is prepared and the work staged on its loops is released; wanted running, the
code drives once the block is ready and `start()` runs (built). A configuration that fails before `configured` is torn
down by 6.9: its staged items are destroyed unrun on the main loop with the loop states they would have run on; a loop
that started destroys its state on its own thread. `waitingFor` (built, `docs/control-port.md`) gains the kinds
`claim` (`subject`, `holder`, and `leaked: true` when the holder is a leaked configuration), `store`, `recording` and
`loop`.

### 6.2 Wakes: the framework's, for now

A thread (the realtime one included) tells a loop "look now", at most once until the loop answers. After 3b the only
wakes are the framework's: a source's clock wake (`timing-model.md` 4.2), and later the status bell (slice 5). Types do not ask for
wakes in 3c (G23).

- **`Wake`** is declared in core (built, `ear/core/source.hpp`; its eventfd is `EventSources`' in `ear_pipewire`):
  `ring() noexcept` does `if (!pending_.exchange(true, acq_rel)) write(efd_, 1);`.
- **Answered** by the loop that watches it (`Loop::onWake`, 6.3), in this order: `read(efd)` (draining it), then
  `pending.store(false, release)`, then the handler. A ring after the drain and before the clear finds `pending` still
  true and writes nothing, but the handler that follows sees its data; a ring after the clear writes the eventfd again.
  So no ring is lost and `pending` is never true with the eventfd empty for good. One watcher per wake.
- **Rung before it is watched.** A source plays from the first drive, before `begin()` arms `onWake`, so arming a watch
  on a pending wake runs the handler once, on the loop's next turn.
- **Implementation.** `EventWake` in `ear_runtime` (the source's eventfd moves here from `ear_pipewire`). Replaces
  sendspin's `EventFd` as the bell.
- **Tests.** The order above on the harness (a ring between drain and clear, by a hook) and on real threads (10^6 rings,
  every ring's data seen, the loop never stuck).

### 6.3 Loops

A serial executor for a node's work off the realtime thread and the main loop (Q5: strands by default, dedicated on
request), declared in the spec (G14), with its state, alive for the node's whole configuration.

```cpp
// ear/core/loop.hpp
namespace loop {
/// Name: 1 to 8 of [a-z0-9-], unique in the spec (checked at compile time). State: the loop's state type, or none.
template <spec_detail::LoopName Name, class State = NoState> inline constexpr LoopHandle<Name, LoopKind::strand, State> strand{};
template <spec_detail::LoopName Name, class State = NoState> inline constexpr LoopHandle<Name, LoopKind::dedicated, State> dedicated{};
}
template <auto... Handles> struct Loops;   // TypeSpec's `.loops` (6.11)

/// A timer or a watch. Destroying it cancels it: its handler never runs after the destructor returns. Destroyed on its
/// loop's thread; or on the main loop while staged; or on any thread once its loop is closed (step 4).
class LoopSource { public: virtual ~LoopSource() = default; };
class Loop : public Executor {
public:
	// Executor::post: any thread; never runs `work` inside the call; serial, in post order. The node's own intake:
	// accepted until step 4. Work posted after it is destroyed unrun, on the caller's thread.
	[[nodiscard]] virtual std::unique_ptr<LoopSource> after(std::chrono::milliseconds delay, std::move_only_function<void()> due) = 0;
	/// Every `period` (CLOCK_MONOTONIC), fixed rate; ticks missed while a handler ran coalesce into one.
	[[nodiscard]] virtual std::unique_ptr<LoopSource> every(std::chrono::milliseconds period, std::move_only_function<void()> due) = 0;
	[[nodiscard]] virtual std::unique_ptr<LoopSource> onWake(Wake& wake, std::move_only_function<void()> rung) = 0;
	/// Each time `fd` is readable. The descriptor is the caller's and outlives the source.
	[[nodiscard]] virtual std::unique_ptr<LoopSource> onReadable(int fd, std::move_only_function<void()> readable) = 0;
	virtual LoopKind kind() const noexcept = 0;
	virtual bool inLoop() const noexcept = 0;
protected:
	~Loop() = default;
};
class NodeLoop : public Loop {   // the framework's side
public:
	/// Main thread. Work from outside the node (slice 5's actions): posted while the loop is open to others, else
	/// destroyed unrun at once (its reply object answers from its destructor).
	virtual void postFromOutside(std::move_only_function<void()> work) = 0;
	/// Main thread, at teardown step 1: refuses outside work from now on, destroys queued outside work unrun on the
	/// loop's thread. The node's own work and sources go on; a handler already running finishes.
	virtual void closeToOthers() = 0;
	/// Main thread, at step 4: takes no more work, runs the work already queued, destroys its state on its own thread,
	/// cancels every source still armed (counting them), then calls `closed(leftSources)` on the main thread.
	virtual void close(std::move_only_function<void(std::size_t leftSources)> closed) = 0;
};
class LoopAdapter {  // ear_runtime: StrandPool and DedicatedLoops behind one adapter
public:
	/// Main thread. The loop runs when `ready` is called, never inside make; `failed` with the reason otherwise.
	virtual std::unique_ptr<NodeLoop> make(const NodeName& node, std::string_view name, LoopKind kind, ReadyEvents& events) = 0;
protected:
	~LoopAdapter() = default;
};
```

- **A loop's state.** Given in creation's `configure` (`cx.loop(worker, std::make_unique<Worker>(...))`, constructed on
  the main loop with no I/O and no threads). From then on the instance holds no reference to it: `NodeOf::post(worker,
  fn)` and `cx.post` run `fn(Worker&)` on the loop (D-A4, I14). The state lives as long as the configuration; it is
  destroyed on its own loop at teardown step 4, where its destructor may join the threads its library owns. A loop
  declared without a state is an executor other states use through `cx.executor(handle)`.
- **Staging, ended at `configured`.** Everything sent to a loop from the main loop before the node is configured (sources
  armed and work posted from `configure`, or from a state's constructor) is staged on the main loop. When the node is
  configured the framework releases the staged items in the order made. If the configuration is torn down first, the
  staged items are destroyed unrun. So no handler runs before every resource the node asked for is ready. Work that must
  wait for the code to drive is posted by `start()`.
- **The source contract.** A state destroys the sources it armed, on their loops, before or in its destructor. The
  framework counts live sources per loop. At step 4 a source still armed after the state is gone is a type bug: the loop
  cancels it itself, the harness fails the test, debug builds assert, and a release build says `node <n>
  ear.loop-source-left: <k> sources were still armed on loop <name> when the node's configuration closed`. (A source
  armed on another loop, as sendspin's worker arms the notifier's `onWake`, is destroyed by a post to that loop.)
- **Two intakes.** From step 1 a loop refuses work from outside the node (`closeToOthers`): slice 5's queued action
  calls are then answered `restarted` or `stopping`, and one already running finishes and replies before the goodbye
  `stop()` posted behind it. The node's own posts and sources are accepted until step 4.
- **Threads.** A handler runs on the loop's thread, one at a time per loop. A strand runs on a framework pool thread
  (`min(4, hardware threads)` to start, G9); a dedicated loop has its own thread, named `ear-<name> <node>`, cut to 15
  bytes (`ear-sendspin <node>`, `ear-notify <node>`, as today). Nothing here is realtime. A dedicated loop may block (a
  library's poll, a disk write, a source write with patience); a strand must not: debug builds assert at every
  blocking primitive (`SourceWriter::write` and `idle`, `Store::replace`) that the caller is not a strand.
- **A slow strand.** A handler past 100 ms is `node <n> ear.loop-slow: loop <name> ran one handler for <ms> ms`. Past
  1 s the pool starts a replacement thread (at most 16 in all), so one stuck strand delays no other node (R16). A
  dedicated loop is never judged.
- **Lifetime.** Started at readiness step 3; closed to others at step 1; at step 4 it runs what was posted, destroys its
  state on its own thread and closes; freed at step 5, after the instance. A dedicated loop's thread is joined at step
  5, after it said it exited, so the join returns at once. A loop of a configuration that never reached step 3 is never
  started; its state, if given, is destroyed on the main loop.
- **What a loop is not.** No priority, no affinity, no realtime scheduling, no futures, no sharing across nodes.
- **Replaces** `SendspinWorker`'s two `std::thread`s, its stop `EventFd`, `waitForStop` and the 10 ms `poll()` loops.
  `ConnectionWatch` stays the type's: a pure judge the tick feeds.

### 6.4 A source's writer: one at a time

The writer's interface and closing contract are built (`docs/writing-a-node-type.md#sources`). This slice adds the rule its users must keep:

- **One writer at a time, handed over by a join or a post.** The ring is single-producer (`FrameRing`). A type may
  change which thread writes (3d: sendspin's new client brings a new sync thread), but only after the old writer's last
  `write` happens-before the new writer's first: the old thread joined (`client->stop()` joins the library's threads,
  `client.h:341-343` [V]), or a post from the old writer's loop to the new one's. Debug builds record the writing
  thread's id and assert when a second thread writes while the first has written within the last cycle and was not
  handed over (`writer.handOver()`, a no-op in release builds, marks it).
- **Harness.** The fake source ring's writer never waits (patience ignored, returns what fits; `idle` returns at once),
  so a test on one thread cannot hang.
- **Replaces** `Feed`'s writer side and the stop-eventfd wait in `on_audio_write`.

### 6.5 A sink's consumer

`SinkConsumer` (`begin`, `consume`, `finish`) is built (`sink.hpp`). This slice adds `consumedBy(loop, &State::member)`
and `consumedBy(loop)`, so a consumer may be a member of a loop's state (sendspin's `ToServer`, which then reaches the
library's source role on the worker's state) or the state itself (the RTP sender). As a sink's consumer `ToServer`
takes its upstream: it dates each piece by its chunk, `FrameClock::enteredRawNs(chunk.startRawNs, offset,
{0, chunk.upstreamFrames})` (`docs/engine.md#dating`), so a card's capture reaches the server dated by when it entered
the graph, not by its cycle's start (`docs/node-types.md#clock`), and the capture anchor goes.
The consumer's `begin` still runs on
the sink's thread before the node is configured, and a stop's silence still unbinds it without telling it
(it sees the gap in positions). `SinkControl` (`pause`, `resume`) is 3d's (`reconfigure.md` 6.4).

### 6.6 The I/O executor

The framework's one thread for disk work, so a slow disk delays no node's loop and never the main loop. Shared by every
node, and bounded.

```cpp
class IoExecutor : public Executor { protected: ~IoExecutor() = default; };   // ear_runtime: IoThread, made at boot
```

- Types never see it. In 3c it opens stores and recordings (readiness step 2) and runs every store write; slice 8's
  `/proc` reads and slice 9's runtime files will use it.
- **Jobs hold shared state.** Every job holds a `shared_ptr` to the state it fills (the store's text and mode, the
  recording's descriptor), never a `NodeStore`, `NodeRecording` or `ReadyEvents` pointer. Its completion is posted to
  the main loop and checked there against the configuration's generation and liveness: a completion for a
  configuration that is gone, or past its deadline, is dropped, and a descriptor it opened is closed by a second job.
- **One thread**: a hung disk stalls every open and every store write. Each has a 5 s deadline, so a node fails visibly
  instead of starting forever (G13).
- **Stopping.** `main.cpp` asks it to stop after `NodeHost::stopAll` completed and the keeper's thread was joined:
  queued jobs are destroyed unrun, and the job in flight gets 2 s. If it is still busy, ear writes a `log` record
  (`source` `ear`, `warn`: `the I/O thread was still busy at exit: <job>`) and leaves with `std::_Exit(0)` once the
  output queue is written, as it does for a leaked configuration. `ear.conf` is written by then.

### 6.7 Stores

A file that belongs to one node and survives its configurations: named by the framework, kept narrow, read and trusted
before any of the node's loop work runs, replaced only if unchanged, swept at boot.

```cpp
// ear/core/type_spec.hpp
struct StoreRule {
	std::uint32_t mode = 0600;                                  // the most it may be; wider is narrowed, never refused
	std::uint32_t maxBytes = 0;                                 // required: a larger file is not read (not trusted)
	std::string (*initial)() = nullptr;                         // the text a missing file is created with; null: none
	std::optional<std::string> (*distrust)(std::string_view) = nullptr;   // why a text is not trusted; null: any text
};
namespace store {
/// Refused at compile time: maxBytes 0 (left out), above 1 MiB, a mode with bits outside 0777.
template <spec_detail::BankName Suffix> consteval StoreHandle<Suffix> file(StoreRule rule);
}
template <auto... Handles> struct Stores;                       // unique suffixes, checked at compile time

class Store {
public:
	virtual const std::string& fileName() const noexcept = 0;   // "<type>-<node>.<suffix>"
	/// What the file held when opened and trusted (or created with); then the text of the last write that committed.
	virtual std::string text() const = 0;
	/// Blocking: a dedicated loop or a library's thread, never a strand, the main loop or the realtime thread. Queued on
	/// the I/O executor behind earlier writes of this store: narrows the mode, swaps `text` in only if the file still
	/// holds text(). At most 5 s. Error: a sentence naming the file ("<file>: changed by something else", "<file>:
	/// larger than <n> bytes", "<file>: not written within 5 s"), the file left as it was.
	virtual std::expected<void, std::string> replace(std::string text) = 0;
	/// The same without waiting, from any of the node's loops; `done` runs on `loop`, or never once the loop closed.
	virtual void replaceLater(std::string text, Loop& loop, std::move_only_function<void(std::expected<void, std::string>)> done) = 0;
protected:
	~Store() = default;
};
class StoreAdapter {   // ear_runtime: ConfinedStores, over the graph file folder (ear_folder) and the I/O executor
public:
	/// Boot, before any node: narrows every "<type>-*.<suffix>" (and its staging file) of every declared store.
	virtual std::vector<SweepFinding> sweep(std::span<const std::unique_ptr<const NodeType>> types) = 0;
	/// Opens on the I/O executor: read (no symlink, at most maxBytes), narrow, judge with `distrust`, create with
	/// `initial()` when missing; `events.ready()` or `events.failed("<file>: <why>")` on the main thread.
	virtual std::unique_ptr<NodeStore> open(const NodeName& node, std::string_view type, const StoreDeclaration& store,
	                                       ReadyEvents& events) = 0;
protected:
	~StoreAdapter() = default;
};
```

- `NodeType` gains `std::span<const StoreDeclaration> stores() const noexcept`, answered from the spec, not virtual.
- **Writes are serialised**: one I/O job per write, in order, each against the text the one before committed, so
  "changed by something else" means something outside ear.
- **Names and folder.** `<type>-<node>.<suffix>` in the graph file folder: sendspin's is `sendspin-<node>.key`, today's
  name, so no file moves; the keeper's watch ignores other names (built).
- **Open** at readiness step 2, after the store's claim was granted (6.8). The failures are today's sendspin texts, now
  the framework's, so the system tests keep their lines (`node ma failed start-failed: sendspin-ma.key: malformed: not
  JSON`): the framework writes `<file>: <why>`, sendspin's `distrust` returns `malformed: not JSON`.
- **Mode.** Narrowed at the boot sweep, at every open (before the text is judged: a refused file may hold a private
  key), and before every write: today's three narrowings, one rule for every store.
- **Never a new identity over distrusted text.** A file `distrust` refuses is left as it was and fails the node; only a
  missing file is created.
- **Boot sweep findings**: a `log` record at `warn` with `source` the type's name, as built (G11).
- **Exclusive**: its implicit claim `store:<file name>` covers the leaked configuration a name's one-at-a-time rule does
  not.
- **Replaces** `NodeKeyStore::open`, `narrowKeyFiles`, the boot sweep's call and log, and `NodeFolders` entirely.
  `NodeKeyStore` keeps only the library's provider interface (`load_blob`, `save_blob`) over the parsed record,
  writing with `Store::replace` from the worker or a library thread.

### 6.8 Claims

Something ear-wide that one configuration at a time may hold: a listening TCP port, a UDP port, and, implicitly, a
store's file and a recording's path.

```cpp
namespace claim {
/// Each takes a checked whole number (a row's value); outside 1 to 65535 is a type bug (the node fails start-failed).
struct TcpListen { explicit TcpListen(std::int64_t port); std::uint16_t port; };   // "cannot listen on port <p>: <why>"
struct Udp { explicit Udp(std::int64_t port); std::uint16_t port; };               // "cannot bind UDP port <p>: <why>"
}
class PortProbe {   // ear_runtime: KernelPortProbe; main thread, non-blocking socket, bind, close
public:
	/// TCP: all addresses with SO_REUSEADDR, so a TIME_WAIT passes and a live listener refuses (as port_claims.cpp).
	virtual std::expected<void, std::string> tcpListen(std::uint16_t port) = 0;
	/// UDP: neither SO_REUSEADDR nor SO_REUSEPORT. rtp-source binds with SO_REUSEADDR
	/// (pipewire-1.4.2/src/modules/module-rtp-source.c:252) [V], so a probe that set it would pass while a receiver holds it.
	virtual std::expected<void, std::string> udp(std::uint16_t port) = 0;
protected:
	~PortProbe() = default;
};
```

The registry is core's (`ClaimRegistry`, in `NodeHost`, main loop only), keyed by kind and value: `tcp:8928`,
`udp:5006`, `store:sendspin-ma.key`, `recording:take1.wav` (a recording's key is its path with `.` and empty components
removed). `udp:<p>` is also claimed implicitly by every RTP receiver companion (`rtp`).

**Judged at readiness step 1**, against the key's holder:

| Holder | Outcome | Detail (when it fails) |
|---|---|---|
| none | granted, then probed (ports only) | the kernel's reason |
| a configuration that is not going (a node running, stopped or configuring) | refused at once | `held by node <n>` |
| a disabled node | — (it holds no claim; an enable judges its claims afresh) | — |
| a configuration that is **going** (between teardown steps 1 and 5, of any node: a restart's, a failure's, a removal's or a rename's old one) | waits: `ear.claim-wait` says why; granted when its step 5 releases it, then probed | after 15 s: `held by node <n>, which did not let go within 15 s` |
| a leaked configuration, port | abandoned: probed afresh; granted if it passes | the kernel's reason, recorded against the leaked record (6.9), so its late close starts this node (R15) |
| a leaked configuration, store or recording | refused | `held by node <n>'s earlier configuration, which did not close; it starts when that closes` (R15) |

- **The wait, visible**: `node <b> ear.claim-wait: <key> held by node <a>, which is closing` (a `diagnostic` record),
  and `waitingFor` `claim` on `node.state`. The holder's close deadline (10 s) passes before the waiter's (15 s), so a
  wait ends in a grant, a port's abandonment, or a refusal naming the leaked configuration.
- **A node never waits on its own old configuration**: its `configure` comes after that one's step 5 (built: one
  configuration at a time). Two nodes whose restarts swap ports both run, whichever teardown finishes first.
- **Release**: at step 5, last. A waiter is granted in arrival order; a waiter torn down before its grant leaves the
  queue at once.
- **The probe's limit stays**: a program taking the port just after the probe is not caught; sendspin's library retries
  every 5 s, an `rtp` receiver logs PipeWire's bind failure as today (and fails at 3b's readiness deadline).
- **mDNS names are not claimed**: sendspin's instance name is `ear-<node>-<first 8 of client id>`, the client id from
  the node's own key file, so two nodes cannot advertise one name.
- **Replaces** `PortClaims`, `HeldPort`, `probeListenPort` and their type-level scope: a claim now spans types.

### 6.9 Teardown, full, and the leaked configuration

The built teardown's steps (`docs/architecture.md`), with this slice's primitives in place. Every step that waits on
another thread is asynchronous; the main loop never joins a thread that has not already said it is done.

| Step | What | Threads it waits on |
|---|---|---|
| 1 | `stop()` if `start()` was called and not answered (built); the fade and ear's barrier (built), which unbinds every ring companion; `release`; every source writer closed; every loop closed to others (queued actions answered); if the node is not configured yet: its claim waits leave the queue, its pending opens are orphaned (6.6), and readiness steps it had not reached never start | the data loop, asynchronously, within the built bounds |
| 2 | Companions destroyed | none |
| 3 | Sinks drained: each hands what its ring holds to its consumer and calls `finish` on its own thread | sinks' threads |
| 4 | Each loop closes: it takes no new work, runs the work already posted (`stop()`'s goodbye), destroys its state on its own thread (where a library's threads are joined), cancels and counts any source still armed (`ear.loop-source-left`), and reports | loop threads, and the threads a state joins |
| 5 | On the main loop: the code; the instance; the companions' rings and wakes; loops (a dedicated thread joined, already exited); stores; recordings closed; claims released (waiters granted); the config | none |

A configuration torn down before it was ready runs step 1 without `stop()`, steps 2 and 3 for what exists, closes at
step 4 whatever loops readiness had started (their staged items destroyed unrun), destroys on the main loop the states
whose loop never started, then step 5. A stop is step 1's first half only (built); a disable is the whole table (built
semantics; its claims released at step 5, so a waiter is granted then).

**The close deadline and the leaked configuration** (R15). The deadline (10 s, built) covers steps 3 and 4. A
configuration past it is leaked (built: never freed, `ear.close-overdue`); 3c adds that the diagnostic names what was
still open (`<n> sinks open, <k> loops open`), its port claims are abandoned and its store and recording claims stay,
and **it keeps listening**:

- Its handle is re-pointed at a **leaked-configuration record** `NodeHost` keeps (a list, never freed); every later
  sink report and loop close lands there, in order on the main loop. The record carries on where the teardown stopped
  (a configuration leaked in step 3 has its loops closed once its last sink reports).
- It lists every node its store and recording claims refused, and every node a fresh probe of one of its abandoned
  ports refused (G27).
- When everything it held has reported, its claims are released, `node <n> ear.close-late: the configuration leaked at
  the close deadline closed after <s> s; its claims are released` is said, and every node its claims refused is started
  again, once, as `node.restart` would (R15). Its memory stays leaked.
- `NodeHost::leaked()` counts leaked configurations still open; one that closed late no longer counts, so a stop after
  it ends normally rather than through `std::_Exit`.

### 6.10 Recordings, deferred

A file beneath the recordings folder that one configuration writes, opened after its claim and off the main loop,
written by a sink's consumer.

```cpp
struct RecordingRequest { std::string path; };   // unchanged: relative to the recordings folder
// configure: Recording& file = cx.recording({path});  claims recording:<normalised path>; its descriptor, created
//                                                     or truncated, valid from the consumer's begin (section 0.3)
class RecordingAdapter {   // amended; main.cpp's adapter over ear_folder
public:
	/// Main thread. Creates or truncates `path` beneath the folder on the I/O executor (openat2, RESOLVE_BENEATH, as
	/// built); `events.ready()` with the descriptor in the node's shared state, or `events.failed(<why>)`.
	virtual std::unique_ptr<NodeRecording> open(const NodeName& node, const std::string& path, ReadyEvents& events) = 0;
protected:
	~RecordingAdapter() = default;
};
```

- **Order.** Asked for in `configure`, judged as a claim at step 1, opened at step 2, the WAV header written in the sink
  consumer's `begin` before the node is configured (built). So a path held by a running node is refused before anything
  is truncated, and a path held by a node still closing waits: removing `rec` and adding `rec2` on `take1.wav` in one
  plan works.
- **Failure text.** `cannot open <path>: <why>`, today's, so the system tests keep their lines; a claim refusal is
  `cannot open <path>: held by node <n>`; a failure becomes a readiness failure instead of `Configured::failed`.
- **`file` on it.** Its `configure` replaces 3a's `openRecording` with `cx.recording({path})`; the rest is unchanged
  (its `WavWriter`, given whole, opens the WAV in `begin`). `path` stays restart-only.
- **Lifetime.** Closed at step 5, after stores and before claims are released; leaked with a leaked configuration, its
  claim kept.

### 6.11 `TypeSpec`, whole: the one home of its members

Every template parameter after `BankList` defaults to an empty list, so a spec gives only what it uses, in this order
(designated initialisers follow declaration order):

```cpp
template <class Settings, class BankList, class StoreList = Stores<>, class LoopList = Loops<>,
          class Status = StatusTable<>, class Events = EventTable<>, class Actions = ActionTable<>,
          class Logs = LogCodes<>>
struct TypeSpec {
	Settings settings;      // built (slice 2)
	BankList banks;         // built (slice 2)
	StoreList stores{};     // slice 3c (6.7)
	LoopList loops{};       // slice 3c (6.3)
	Lifecycle lifecycle{};  // slice 3d (reconfigure.md): the in-place deadline, 1 s to 60 s
	Status status{};        // slice 5 (control-plane.md)
	Events events{};        // slice 5
	Actions actions{};      // slice 5
	Logs logCodes{};        // slice 5
};
```

`Stores`, `Loops` and `Banks` check their names' uniqueness at compile time; a store, a loop and a bank may share a
name. Whether GCC 14 deduces the defaulted parameters for every subset spelling is U1.

## 7. Threads, as one contract

| What the type writes | Runs on | May |
|---|---|---|
| Rows' parse functions, `check`, `ports`, a store's `initial` and `distrust` | any thread, pure | nothing else (`initial` may read the clock and a random source) |
| The constructor, `configure` (and the loop states' constructors it calls) | the main loop | acquire; take store handles, writers, executors and the node handle; give loops their states; name consumers; post (staged). No I/O, no threads, no blocking |
| `start()`, `stop()` | the main loop | post to the node's loops |
| `prepare`, `release` | the main loop, never with `process` | allocate |
| `process` | the realtime thread | `io` only |
| Strand handlers | a pool thread, serially per strand | anything that does not block: `post`, `replaceLater`, `NodeHandle`, non-blocking sockets |
| Dedicated loop handlers | the loop's thread | also block: `SourceWriter::write` and `idle`, `Store::replace`, a library's `start`, `stop` and poll |
| Threads a library owns | their own | what the library does, `SourceWriter::write` and `idle`, `Store::replace`; the loop's state joins them when it is destroyed, on its loop |
| `SinkConsumer::begin`, `consume`, `finish` | the sink's thread | block on I/O |

A guard joins `standard_output_guard.cmake` as `thread_guard.cmake`: no non-test file under `src/nodes/` (it skips every
path with a `/tests/` component) names `std::thread`, `std::jthread`, `std::async`, `pthread_create` or `fork`. Its own
test covers each spelling, a look-alike (`my_thread_count`) and the `/tests/` exclusion (G12).

## 8. Invariants, failure modes and visible changes

Invariants this slice adds to the built ones, each a test's to prove:

- **P1.** No type code starts a thread, and no type code does I/O on the main loop.
- **P2.** No handler of a node's loops runs before the node is configured or after its loop's state is destroyed, and
  every loop of a configuration is closed before its instance is freed.
- **P3.** A claim has one holder ear-wide, across types, judged before anything of the configuration is opened or
  started; a leaked configuration's claim on something no probe can check is never handed on before its late close.
- **P4.** A store's file is never wider than its mode after boot, an open or a write, and never replaced by a new
  identity when its text is distrusted.
- **P5.** What a configuration keeps, a stop and a start keep: loops and their states, stores, the handle, claims.
- **P6.** One writer of a source and one consumer call of a sink at a time; a writer changes only by a join or a post.

| Failure | What the operator sees | Why it is safe |
|---|---|---|
| A store cannot open (symlink, directory, too big, unreadable, distrusted, cannot be created) | `node <n> starting`, then `failed start-failed: <file>: <why>`; the file unchanged | no loop work ran, no loop started |
| A store or recording open hangs | after 5 s, `failed start-failed: <file>: not opened within 5 s` | the job holds only shared state; its late completion is dropped |
| A later store write is refused | sendspin: `says key-write-failed <detail>` (today's note, its text from the store) | the file keeps the other writer's text |
| A claim held by another running node | `failed start-failed: cannot listen on port <p>: held by node <n>` (today's text); `cannot open take1.wav: held by node <n>` for a path | refused at step 1: nothing opened or started |
| A claim held by a node still closing | `starting`, `ear.claim-wait: ...`, then `running`; after 15 s, failed `... which did not let go within 15 s` | the holder's close deadline passes first |
| A port another program holds | `failed start-failed: cannot listen on port <p>: Address already in use` (today's); for `rtp`, `cannot bind UDP port <p>: Address already in use` | the probe |
| A claim held by a leaked configuration | port: probed afresh; store or path: refused naming the leaked configuration; later `ear.close-late` and the node running by itself | R15 |
| A strand handler that never returns | `ear.loop-slow` after 100 ms; a replacement pool thread after 1 s; at teardown the close deadline leaks the configuration | other nodes' strands keep their threads |
| A dedicated loop that never drains | at teardown, after 10 s, `ear.close-overdue ... 1 loops open` | leaked, never freed |
| A loop's thread cannot start | `failed start-failed: cannot start loop <name>: <errno text>` | no loop work ran |
| A loop state that leaves a source armed | `ear.loop-source-left`; the harness and debug builds fail | the loop cancels it at step 4 |
| A post, ring or handle call after the configuration closed | dropped | loops destroy late work unrun; the handle is detached |
| The I/O thread busy at exit | `the I/O thread was still busy at exit: <job>`, then `std::_Exit(0)` | `ear.conf` was written first |

**Visible changes**, each with a system test in its step:

1. **A corrupt key file** fails sendspin before it was `running` (today the worker reads the file after the recipe
   returned). The system test checks only the `failed` line (`sendspin_pairing_test.cpp`) (U4).
2. **Two `file` nodes on one path**: today both run and corrupt one file. Now the second fails `start-failed: cannot
   open take1.wav: held by node <first>`; removing one and adding the other in one plan works.
3. **An `rtp` node on a UDP port another program holds** fails `start-failed: cannot bind UDP port 5004: Address
   already in use` at once (after 3b it fails at its 5 s readiness deadline; before 3b it waited for good). Two `rtp`
   nodes on one port are refused `held by node <n>`; two multicast groups on one port too (section 15).
4. **New diagnostics**: `ear.claim-wait`, `ear.close-late`, `ear.loop-slow`, `ear.loop-source-left`; `ear.close-overdue`
   gains its counts. Each is in `test/protocol/output-records.json` from the step that adds it.
5. **A stopped `sendspin` node closes its connection** (its `stop()` posts the goodbye), where on the legacy adapter it
   kept it; Start then reconnects.

## 9. The carried sendspin-cpp log patch

**Today.** On a host build `SS_LOG*` is a bare `fprintf(stderr, ...)` behind one process-wide level
(`src/platform/logging.h:56-63` in the pinned tree) [V]. ear sets `WARN`; its lines reach standard output as
unattributed `stderr` records at `warn`, and with two sendspin nodes nobody can tell whose. There are 533 `SS_LOG*`
call sites in 36 files [V]; none takes a client.

**The patch** (`cmake/sendspin-log.patch`, beside `sendspin-listener.patch`):

1. `logging.h`, host branch: the five macros call `::sendspin::log_emit(level, tag, fmt, ...)`
   (`__attribute__((format(printf, 3, 4)))`), defined in a new `src/platform/logging.cpp`. It checks the level as now,
   formats into a 512-byte stack buffer (a longer line cut and marked), and calls the sink if one is set, else writes
   `stderr` exactly as today. No allocation.
2. The sink is process-wide, the context per thread:
   ```cpp
   namespace sendspin {
   using LogSink = void (*)(void* context, int level, const char* tag, const char* text, bool cut) noexcept;
   void set_log_sink(LogSink sink) noexcept;                 // atomic; null restores stderr
   class LogScope { public: explicit LogScope(void* context) noexcept; ~LogScope(); };   // this thread's context
   }
   ```
3. `SendspinClientConfig::log_context` (`void*`, default null). The client opens a `LogScope` with it in every public
   entry point (`start`, `loop`, `stop`, `connect_to`, `disconnect`, the destructor, `PlayerRole::notify_audio_played`,
   `SourceRole::write_audio`), so callbacks it makes from there, the persistence provider's included, inherit it.
4. The three library threads (`ProtocolTask::start`, `SyncTask::start`, `SourceTask::start`) capture the caller's
   context and open a scope at the thread's entry; they are started inside `client.start()`, which is scoped.
5. The three IXWebSocket callbacks the library registers capture the context and open a scope in the lambda.
   IXWebSocket's own `fprintf`s stay unattributed `stderr` records (`ixwebsocket-limits.patch` already bounds them).

**ear's side**, in sendspin's worker state: `set_log_sink(&forward)` once per process, and `config.log_context =
&handle_` (the node's `NodeHandle`). `forward` maps `SS_LOG_ERROR`, `WARN`, `INFO` and `DEBUG`/`VERBOSE` to ear's four
levels and calls `handle.log({.level, .source = "sendspin-cpp", .text, .topic = tag, .cut})`. A null context is an
unattributed `log` record from `sendspin-cpp`. The context is the node handle, which outlives the configuration; each
client is destroyed on the worker before the worker's state is, so no thread of an old client logs after it is gone.
Configure refuses a tree without the patch (`cmake/CarriedPatch.cmake`, built); `sendspin_patched_dep` takes a list of
patches. Offering it upstream is worth a try (U8).

**What it retires in the tests and the counts** (moved here when the cycle-facts design was retired, its steps all
built). The `sendspin` clock tests match a library line within an interim reach, `kLogReach` and `kLogLag`, and
stretch a loss of sync by `kLogLag` (`inLoss`), because a line read from standard error is stamped when it was read,
not where its audio plays (`docs/testing.md#the-librarys-own-word`); with the patch, ear's sink stamps each line with
the feed's write position, so a line names the frame it acts on and the three go. A count of the library's re-locks,
when ear keeps one, is the node's status (`control-plane.md` 3.1), not a framework counter; and a legacy handoff's
drops, in `node.gone`'s `droppedFrames` today, become the sink companion's `dropped` when S3c.6 retires the handoff.

## 10. The type harness, for slice 3c

`TypeHarness<spec, Type>` runs a type over a real `NodeHost` with in-memory fakes (built: `parse`, `add`,
`blockReady`, `ready`, `lose`, `process`, `stop`, `start`, `restart`, `change`, `disable`, `enable`, `remove`,
`phase()`, `health()`, `companion(bank)`). This slice adds the fakes of its primitives, each named by the spec's handle
(a misnamed or undeclared handle does not compile):

| Kind | The fake | What a test does |
|---|---|---|
| Loop | a queue and a fake clock per loop; kind kept, so blocking asserts still judge strands | `runLoops()`; `advance(ms)`; `loop(worker)` to turn one; `armed(worker)` counts its live sources |
| Store | in memory, by handle | `presetStore(keyFile, text)`, `failStoreOpen(keyFile, why)`, `failNextWrite(keyFile, why)`, `storeText(keyFile)` |
| Recording | a real descriptor on a temporary file, or a failure (replaces 3a's `Options::recordings`) | `failRecording(path, why)`, `recordingText(path)` |
| Claim | granted by default | `holdClaim(claim::TcpListen{8928}, "porch")`, `waitClaim(...)`, `releaseClaim(...)`, `failProbe(...)`; `claims()` |
| Readable descriptor | delivered by the test | `readable(fd)`: the `onReadable` handler runs on its loop's next turn |
| Node handle | captured | `notes()` (paced as the framework paces), `logs()`, the fact |
| Source writer | on 3b's fake source; never waits | `writerThreads(bank)` in debug builds, for P6 |

Rules a test can rely on: `add()` makes the instance, calls `configure`, judges claims and opens stores and recordings
at once, then starts the loops with their states and calls every consumer's `begin`, unless the test made a claim wait
or an open fail first (after `releaseClaim`, readiness carries on at once); once configured, staged items are released
and run on the next `runLoops()`. `runLoops()` runs posted work, rung wakes and readable deliveries until all loops are
idle and never fires a timer; `advance(d)` moves the fake clock and fires, in time order, every timer due. A `replace`
completes inside the call, a `replaceLater` on the next `runLoops()`. `remove()` (and a restart, a disable) runs the
teardown as the framework does and turns each loop until its posted work ran and its state is destroyed; if a state,
once destroyed, leaves sources armed, the test fails naming the loop. So sendspin's `begin()` runs only on `runLoops()`
after `blockReady()`: the all-type tests run sendspin without a network, and `notYetInHarness` goes.

## 11. Where the code lives

| Package | Gains | Never |
|---|---|---|
| `ear_core` | `node_handle.hpp`, `loop.hpp`, the store, recording, claim and probe interfaces, `IoExecutor`; `ClaimRegistry` and the leaked-configuration record in `NodeHost`; the contexts' members of 6.1, `StoreRule`, `Stores`, `store::file`, `Loops`, `loop::strand`, `loop::dedicated` and `TypeSpec`'s new members; `NodeType::stores()` | threads, files, sockets |
| `ear_runtime` (`src/runtime`, new) | `StrandPool` and `DedicatedLoops` (one `LoopAdapter`), `EventWake` (moved from `ear_pipewire`), `IoThread`, `KernelPortProbe`, `ConfinedStores` (store adapter and boot sweep, over `ear_folder`) | PipeWire; policy (what a claim means, when a store is trusted) |
| `ear_type_harness` | the fakes of section 10 | PipeWire, real threads |
| `ear_node_sendspin` | loses its primitives (section 12) | `std::thread` (the guard) |
| `ear_all_node_types` | `allNodeTypes()` takes no argument | — |
| `ear` (`main.cpp`) | makes the `ear_runtime` adapters and the I/O executor, sweeps at boot (findings to `StatusLog`), passes them to `ResourceRegistry`, `nodes.logTo(log)`, stops the I/O executor last; the recordings adapter opens on the I/O executor | computing anything |

Dependencies: `ear_core` <- `ear_runtime` -> `ear_folder`; `ear_runtime` <- `ear`. No node type links `ear_runtime` or
`ear_pipewire`: S3c.2 asks configure's `ear_refuse_node_type_links` to refuse those two packages too.
`ResourceRegistry` takes `ResourceAdapters{...}`, one member per kind; a null adapter refuses its kind with "no <kind>
in this host" (as a missing recordings folder does today).

## 12. sendspin and file, file by file

sendspin's namespace becomes `ear::nodes::sendspin`. S3c.6 makes every change below in one step from today's code.

| File | Fate |
|---|---|
| `include/ear/nodes/sendspin.hpp` | `makeSendspinNodeType()`, no argument |
| `feed.*`, `sendspin_code.*`, the capture anchor | deleted: a source on `from_server` and a sink on `to_server`; `sendspin_code.*`'s playback half (T2's honest wrapper) becomes `sendspin_clock.*`, the source's clock (`timing-model.md` 6) |
| `mailbox.hpp` | kept: the clock's report (`PlayProgress`) to the notifier |
| `sendspin_type.cpp` | renamed `sendspin.cpp`: the class `Sendspin`, `makeSendspinNodeType`. `SendspinConfig`, `SendspinType`, `HeldPort` and the boot sweep go |
| new `sendspin_spec.hpp` | rows (moved from `settings.hpp`), banks, `keyFile`, `worker`, `notifier`, `spec`, `Sendspin`, `feedFramesFor`, `emptyKeyRecord`, `whyNotKeyRecord` |
| `settings.*` | `ServerUrl`, `parseName`, `SendspinSettings`, `settingsFrom`; namespace only |
| `worker.*` | `Worker`, the worker loop's state, built from `WorkerParts` in `configure`, with `begin` and `end`. Gone: both `std::thread`s, the stop `EventFd`, `waitForStop`, `poll()`, the joins, `NoteGate`. `clientMutex_` stays: a stop's goodbye (`end()`, on the worker) can destroy the client while the sink's thread is still draining into `write_audio`, so the lock guards it until 3d's `SinkControl::pause` replaces it (`reconfigure.md` 6.4). About 320 lines |
| `key_store.*` | `NodeKeyStore` keeps `load_blob`, `save_blob` over a parsed `KeyRecord` and a `Store&`; `whyMalformed` becomes `whyNotKeyRecord`; `open` and `narrowKeyFiles` go |
| `event_fd.hpp`, `note_gate.*`, `port_claims.*` | deleted |
| `to_server.*` | `ToServer : SinkConsumer` over stamped chunks, a member of the worker's state |
| `connection_watch.*`, `advertiser.*`, `pcm.hpp`, `peer_text.hpp`, `notes.hpp` | namespace only; `notes::notesDropped` goes (the framework says it) |

`file`: its `configure` replaces `openRecording` with `cx.recording({path})`; its tests use the harness's recording
fake.

| sendspin test file | Fate |
|---|---|
| `code_test.cpp`, `feed_test.cpp`, `to_server_test.cpp` | the skip and dating cases become the clock's unit rows (`timing-model.md` 6.4) through the harness, the ring's are the source's (3b); `to_server`'s adapt to stamped chunks |
| `port_claims_test.cpp` | the claim case moves to core (`ClaimRegistry`); the probe cases to `ear_runtime`, plus a UDP port held with SO_REUSEADDR refused |
| `notes_test.cpp` | `NoteGate`'s cases move to core's node handle tests; `printable` and the texts stay |
| `keys_test.cpp` | open, narrow, create, symlink, size and replace cases move to `ear_runtime` (`ConfinedStores`); the record's own cases stay, over a harness store |
| `settings_test.cpp` | the type case through `TypeHarness<spec, Sendspin>` |
| `clock_test.cpp` | builds the node's parts with `ear_runtime`'s real adapters beside its real engine |
| new `type_test.cpp` | listening asks `claim::TcpListen{8928}`, dialling asks none; a held claim fails with today's text; a distrusted store fails with today's text and is not written; the worker's first timer starts the client only once `start` ran and the clock advanced; close runs the goodbye on the worker after the notifier let go, with no source left |
| `test/nodes/all_types.hpp` | `notYetInHarness` deleted; `allTypes()` calls `allNodeTypes()`; the config folder and boot log go |
| `test/system/*` | unchanged lines; S3c.6 checks every sendspin system test on the new order of states (U4) |

## 13. The cut: buildable steps

Superseded by section 0.5, which re-cuts these steps against today's code; the test lists below are
the ones 0.5 cites.

Each step keeps every test green and changes no audio path.

| Step | What | Tests it adds |
|---|---|---|
| **S3c.1 Node handle** (done: 26d2bf56cf) | `NodeHandle` replaces `InsidesEvents`; `node.handle()`; `NodeHost::logTo`; `say` paced in core per node (sendspin's `NoteGate` stays until S3c.6, so its notes are paced twice, harmlessly) | a node's log carries its node; a log after detach dropped; a late message from an older generation dropped; a goodbye note heard; pacing 32 then 1/s with `notes-dropped`, per node, kept across a stop and a start; `lost` never held back |
| **S3c.2 Registry and I/O thread** | `ear_runtime`; `ResourceAdapters`; `IoThread` with shared-state jobs and its stop; `ear_refuse_node_type_links` refuses `ear_runtime` and `ear_pipewire` | a null adapter refuses its kind; a job's completion for a gone configuration dropped and its descriptor closed; the I/O thread busy at stop; the link refusal on each package |
| **S3c.3 Loops** | `loop.hpp`, `Loops`, `TypeSpec::loops`, `StrandPool`, `DedicatedLoops`; `cx.loop`, `cx.executor`, typed posts; staging until configured; `postFromOutside`, `closeToOthers`; teardown step 4; the source count; `Loop::onWake` on `EventWake`; the harness's loops and clock | post order; `after`, `every` (coalescing), `onWake` (a pending wake fires once on arming), `onReadable`; staged work runs only once configured, never after a configuration that failed first; a state destroyed on its own thread; a post typed for another loop's state does not compile; outside work refused from step 1; a source left armed; a dedicated loop that never drains leaked with "1 loops open"; `ear.loop-slow`; a stuck strand gets a replacement thread; the blocking assert on a strand; the wake's drain-then-clear order |
| **S3c.4 Stores** | `StoreRule`, `Stores`, `TypeSpec::stores`, `NodeType::stores()`; `ConfinedStores` with sweep, open at step 2, serialised writes, the 5 s deadlines | store open (missing, created, symlink, too big, distrusted with the file untouched, mode narrowed before judging); `replace` refused when changed; two writes in flight both commit in order; sweep over declared stores; an open past its deadline dropped; one compile test of every `TypeSpec` subset spelling (U1); compile-fail cases: two stores of one suffix, a bad suffix, `maxBytes` left out, a mode beyond 0777, a store or loop handle the spec lacks |
| **S3c.5 Claims, recordings** | `claim::TcpListen`, `claim::Udp`, `ClaimRegistry` at step 1, `KernelPortProbe`; implicit store, recording and RTP receiver claims; recordings deferred onto the I/O executor; the leaked-configuration record, late close, auto-start (R15); `ear.claim-wait`, `ear.close-late` in `output-records.json`; `waitingFor`'s new kinds | granted; refused (running holder, across types) before any open or thread; waited (a removed node on 8928 and an added one in one plan; `rec` removed and `rec2` added on one path); the 15 s deadline; two nodes whose restarts swap ports both run, teardowns finishing in either order; an abandoned port re-probed; a store and a recording path refused while a leaked configuration holds them, then freed by its late close and the node running by itself (a system test: a peer that never answers at stop); probe cases moved; system tests for visible changes 2 and 3 |
| **S3c.6 sendspin moves** | section 12, from today's SPI in one step: the class, its source (with `SendspinClock`, `timing-model.md` 7) and sink companions, its worker a loop state; the legacy adapter and the old SPI go (`LegacyInstance`, `StartContext`, `StartResult`, `RunRecipe`, `Ready`, `RunObject`, `HandoffRequest`, `RtWriter`, `HandoffConsumer`, `HandoffAdapter`, `ThreadHandoffs`, the `RunCode` alias, `NodeConfig::start` and `make`'s default); `node_folders.hpp`, the folder argument and `notYetInHarness` go; the thread guard | `type_test.cpp`; sendspin in the all-type harness test; the guard on each spelling; every system test (U4); `handoff_test.cpp` goes (its cases are `sink_test.cpp`'s) |
| **S3c.7 The log patch** | `cmake/sendspin-log.patch`; `sendspin_patched_dep` with a patch list; the worker's state sets the sink and context | `ear_carried_patch_check` for both patches; a library line from the worker and one from the sync thread arrive as `log` records with the node (U2); two nodes' lines told apart; a line outside any scope is unattributed |
| **S3c.8 Words** | AGENTS.md, `docs/writing-a-node-type.md`, `docs/node-types.md` (sendspin), the `sendspin` skill; this page deleted, its lasting content moved there | — |

## 14. The boundary with slice 5

| Slice 3c delivers | Slice 5 builds on it (`control-plane.md`) |
|---|---|
| `NodeHandle`: `node`, `lost`, `log`, interim `say`, reached by `node.handle()` | `Report<spec>` over the handle (status writes, events), reached as `node.report()`; removes `say` once sendspin's notes are split |
| Loops declared in the spec; `postFromOutside` and `closeToOthers` at step 1 | action calls posted with `postFromOutside`; a queued call answered `restarted` or `stopping` at step 1; status fields bound to a loop by `writer::loop` |
| "A thread a library owns" as the only non-loop thread | a library thread posts to the loop that owns a status field |
| Diagnostic codes and texts: `ear.loop-slow`, `ear.loop-source-left`, `ear.claim-wait`, `ear.close-late`, `ear.close-overdue` with counts | puts them on the wire (`node.diagnostic`) and in snapshots |
| `notes-dropped <n>` as the framework's note | `ear.events-dropped` |
| `TypeSpec`'s member list and order (6.11) | `.status`, `.events`, `.actions`, `.logCodes` after `.lifecycle` |
| The harness's fakes and its clock | captures status, events, actions and diagnostics; `h.invoke<action>(args)` on the harness's loops |

A store does not appear on the wire.

## 15. Costs and risks

- **A wider core and a new package.** About 500 lines of interfaces, facades and registry in core, about 700 in
  `ear_runtime` (mostly moved from sendspin), against about 450 fewer in sendspin.
- **`ResourceRegistry`'s constructor changes**, so every `NodeHost` test's setup changes once (S3c.2).
- **About ten compile-fail cases** (the existing 84 cost about 230 s summed at `JOBS=4`).
- **One I/O thread.** A hung disk stalls every open and every store write; the 5 s deadlines make it visible.
- **A shared strand pool** can let one node delay another for up to 1 s before a replacement thread starts; sendspin
  uses only dedicated loops, so nothing built runs on a strand yet.
- **A second carried patch** to rebase at each sendspin-cpp bump, touching about 15 places (U2).
- **Leaked configurations keep stores and paths longer** (refused until a late close), so a node behind a library that
  never finishes its stop stays failed until its late close or ear's restart.
- **Claims are by port, not by address.** Two `rtp` nodes on one port with different multicast groups are refused,
  though PipeWire binds each to its group (`module-rtp-source.c:265-309`) [V]. Multicast is untested; a later key
  `udp:<group>:<port>` can relax it.

## 16. Rejected alternatives

| Alternative | Why not |
|---|---|
| Loops on `pw_thread_loop` in `ear_pipewire` | A loop has nothing to do with the engine; strands need a pool; every threaded type's tests would start PipeWire's loop library (G1) |
| The instance owns its threads (no loop primitive) | Against Q5; each type rewrites stop, wake and poll |
| Loops asked for in `configure` by a runtime name | Checked only at run time (G14) |
| A recipe lambda | Every threaded type repacks resources by hand and a dangling capture compiles (G15) |
| Kept values, a feed and a late handoff consumer | Made necessary only by the two-phase start, which 3a removed (R13, R14) |
| Staging released when `start()` returns | Work that only needs its resources (a state reading its store) would wait on the engine |
| A close hook beside `stop()` | R23's three hooks; a state destroyed on its own loop needs no hook (D-A6) |
| A bare reference to a loop's state on the main loop | A data race a hook could write; typed posts make it impossible (I14) |
| Wakes for types now | No type rings one after 3b (G23) |
| Stores opened by `configure` or a loop's state | I/O on the main loop, or a state that reads before the file is trusted |
| Asynchronous store writes only | The library's `save_blob` returns a `bool` synchronously on its thread (G7) |
| A recording claim judged inside `configure`, the open kept there | No wait is possible, so a remove-and-add of one path cannot work (G16) |
| Claims judged after stores open | The open would read, narrow or create the very file a leaked configuration still holds |
| Claims per type; the probe alone | A port is ear-wide; the probe alone loses "held by node <n>" and refuses a remove-and-add |
| A closed source writer returning 0 at once to every caller | sendspin-cpp's sync thread paces itself on the write blocking and would spin |
| Passing the client at each `SS_LOG*` site | 533 sites: a patch no bump could carry (G8) |

## 17. Decisions already taken

Each is the designer's (the owner answers only what an operator or a type author sees, R24); the owner can overturn
any.

| # | Decision | Why |
|---|---|---|
| D-A4 | Hooks run on the main loop; a node's threads are its loops'; a hook reaches a loop only by a post typed by the loop's handle and holds no reference to a loop's state | A hook cannot race a node's thread; a post of the wrong type does not compile |
| D-A6 | A loop's state is destroyed on its own loop, after the work already posted ran; its destructor joins its library's threads; there is no close hook | R23; joining off the main loop keeps hooks non-blocking; the teardown deadline still bounds it |
| D-R15 (G25) | `sendspin` moves from the legacy adapter onto the class, its companions and loops in one step (S3c.6) | One move instead of several; the legacy adapter is deleted whole |
| G5, G11 | The node handle paces every type's `say`; boot findings stay `log` records from the type's name until slice 5 | sendspin deletes `NoteGate` now; slice 5 replaces both texts |
| G7 | A store writes with `replace` (blocking up to 5 s, from a dedicated loop or a library thread) and `replaceLater` (any loop), in order on the I/O thread | sendspin-cpp's persistence callback answers synchronously |
| G8 | A carried sendspin-cpp patch adds a log hook with a per-thread context | Small enough to rebase at each pin bump |
| G9 | The strand pool: `min(4, cores)` threads, slow at 100 ms, a replacement thread at 1 s, at most 16 | One stuck strand delays the others for at most 1 s |
| G12 | A configure-time guard refuses `std::thread`, `std::jthread`, `std::async`, `pthread_create` and `fork` under `src/nodes/` outside tests | A type's threads are its loops' |
| G13 | Opens 5 s; a claim wait 15 s (after the holder's 10 s teardown deadline); a consumer's `begin` 5 s | Every wait gets an end |
| G16 | A recording's path is claimed first, opened on the I/O thread, its header written by the consumer's `begin` before the node is configured | A rename on the same path works; a duplicate path is refused before anything is truncated |
| G23 | Types get no wakes of their own yet; `Wake` and `Loop::onWake` serve the framework's | Nothing would use them |
| G24 | No primitive sees `disabled`: it is never in a type's checked settings, and no store, claim or recording is keyed on it | A disable is the framework's teardown, not a change a type judges |
| G26 | Each member sits in the context of the mode that may call it: `store`, `loop` and `executor` on `CreateOf` only | R33: a call in the wrong mode does not compile |
| G27 | A fresh probe's failure on a port a leaked configuration abandoned is recorded against that record, so its late close restarts the node | Otherwise the refused node could stay failed for good |
| new | A rule a compiler cannot check fails the node `start-failed` with the rule's text, never throws out of `NodeHost` | As built in 3a for every context rule (`docs/writing-a-node-type.md`) |

## 18. Open items

- **U1.** Aggregate deduction for `TypeSpec{.settings, .banks, [.stores], [.loops]}` with defaulted later parameters on
  GCC 14: S3c.4 compiles every subset spelling first; slice 5 extends it. Fallback: a consteval `makeSpec(...)` with the
  same members in the same order.
- **U2.** That every `SS_LOG*` line sendspin's tests provoke runs inside a scope: S3c.7's system test counts
  unattributed `sendspin-cpp` records over the pairing and connection tests; each one found is a missed entry point.
- **U3.** That the library calls the persistence provider only on threads in the client's scope (`client.cpp:1648-1741`,
  `player_role.cpp:751-778`, `record_store.cpp:43` [V] are the call sites; their threads are not traced). S3c.6 asserts
  in `save_blob` that it runs on a dedicated loop or a library thread.
- **U4.** sendspin's system tests and a `running` line before a corrupt key file's failure: S3c.6 runs them.
- **U5.** The strand pool's sizes on the Talos nodes' CPU, unknown until the hardware is.
- **U6.** Whether `onReadable` serves sendspin's mDNS socket better than answering in the 10 ms tick: not needed to move.
- **U7.** The writer's close contract keeps sendspin-cpp's sync thread asleep between calls from step 1 to step 4: 3b's
  source tests prove it on the ring; S3c.6 measures the real sync thread across a teardown held 2 s by a slow consumer.
- **U8.** Upstream interest in the log hook.
