# Slice 3a's remaining steps, module by module: the interfaces implementers build behind

Status: proposal, revision 1, 2026-11-09, not reviewed. Written by the slice's architect at the owner's direction of
2026-11-09 ("At the top of each workflow slice, have a dedicated software architect review the high level doc and
produce a more granular module level design. It should produce explicit interfaces and signatures of the core
components, so that implementers can fill in implementation around the coherent module proposal."), after reading
`docs/design/node-framework.md` (revision 8.2), `docs/design/node-lifecycle.md` (revision 2.7),
`docs/design/control-plane.md` (revision 6.2) and the code at `db6dd28`, with step L-3d's in-flight `node_host.hpp` read
for its `NodeHealth` and `NodeOperations`.

Layer: module interfaces, beneath node-lifecycle.md (slice 3a's module design: the machines, `NodeHost`'s rewrite, the
cut) and node-framework.md 6.10 (the type author's API). It covers the steps node-lifecycle.md section 12 leaves:
**L-4a** (the type's class), **L-4b** (the interim sink), **L-6** (the wire) and what **L-8** closes. Steps L-3d
(`NodeHealth`'s members, `NodeOperations`, `StatusLog`'s new node sentences) and L-7 (`StallWatch`) are being built
elsewhere; their interfaces are taken as node-lifecycle.md and L-3d's header give them (section 1.2).

**What this page is the home of.** The C++ declarations of the modules those steps add or change, with each function's
thread, ownership, errors and the order components call each other in; the four built types written whole against
them; the wire's C++ types and the mapping from core's replies to the wire's codes. It is not the home of any rule:
when a declaration says "as 4.2's table", the rule is node-lifecycle.md's or node-framework.md's, and the wire's shapes
are control-plane.md's (this page repeats a shape only to say which C++ makes it). Where the higher pages are
ambiguous or disagree, section 9 says so and section 8 records the decision; section 10 asks the owner what is theirs.

**The headers.** Four headers land with this page, declarations at their real paths, each compiled on its own by the
repository's header check, none yet included by any translation unit (so nothing changes in behaviour):

| Header | Step | Holds |
|---|---|---|
| `src/core/include/ear/core/lifecycle.hpp` | L-4a | `NodeCode` (alias), `Configured`, `Changed` (3d, declared), `NodeContext`, `SinkRequest`, `CreateContext`, `NodeOf<Spec>`, `CreateOf<Spec>`, `ChangeOf<Spec>` (declared only), `InstanceContexts`, `ErasedInstance`, `spec_detail::SpecInstance<Spec, Type>` with the class's compile-time checks |
| `src/core/include/ear/core/sink.hpp` | L-4b | `Chunk`, `SinkConsumer`, `SinkShape`, `ChunkStamp`, `SinkTap`, `SinkBinding`, `Sink`, `SinkAdapter` |
| `src/core/include/ear/testing/lifecycle_harness.hpp` | L-4a, L-4b | `ear::testing::next::NodeHarness`, `TypeHarness`, `CompanionFake`, `SeenChunk`, `SeenDiagnostic`, `HarnessOptions`, `AddOptions` (interim location, 3.8) |
| `src/control/include/ear/control/lifecycle_wire.hpp` | L-6 | `AppliedKind`, `Applied`, `NodeDisabled`, `NotRestartable`, `ControlRefusal`, `LifecycleVerb`, `ActionRecord`, `ActionListener` |

and `src/pipewire/include/ear/pipewire/handoffs.hpp` gains `ThreadSinks`'s declaration (L-4b). Every other change to an
existing header is given below as old and new, for the step that makes it, because making it now would change what
today's code compiles against or collide with L-3d's edits of the same files.

The bodies in `lifecycle.hpp` and the harness's typed wrappers are forwarding and `static_assert`s only: they are the
type author's contract, and a scratch translation unit compiled the four types of section 3.9 and 4.7 against them and
saw each compile-time refusal of 3.10 fire with its text.

## 1. Before the modules

### 1.1 The module map

| Module | Package (file) | Step | Its thread | Calls | Called by |
|---|---|---|---|---|---|
| The type's class (`White`, `Card`, `Rtp`, `File`) | `ear_node_<type>` (`<type>_spec.hpp`, `<type>.cpp`) | L-4a, L-4b | main loop; a sink consumer on its sink's thread | `CreateOf<spec>`, `NodeOf<spec>` | `SpecInstance` |
| `SpecInstance<Spec, Type>`, the class's checks | `ear_core` (`lifecycle.hpp`) | L-4a | main loop | the type's class | `NodeHost` through `ErasedInstance` |
| `SpecConfig`, `SpecNodeType`, `makeNodeType` | `ear_core` (`lifecycle.hpp`, moved from `type_spec.hpp`) | L-4a | any (parse); main loop (`make`) | `SpecInstance` | `NodeType::parse`, `NodeHost` |
| `LegacyInstance` | `ear_core` (`src/core/legacy_instance.cpp`, private) | L-4a | main loop | `NodeConfig::start`, `RunRecipe`, `RunObject` | `NodeHost` through `ErasedInstance` |
| `NodeHost`'s contexts (`CreateContext`, `StartContext`, `NodeContext`, `SinkRequest` on one set of internals) | `ear_core` (`node_host.cpp`) | L-4a, L-4b | main loop | adapters, `SinkAdapter`, `PortBlock` | `CreateOf`, `LegacyInstance` |
| Hook timing (`ear.hook-slow`) | `ear_core` (`node_host.cpp`) | L-4a | main loop | `Timers::now`, `NodeDiagnosticListener` | `NodeHost` around each hook |
| The interim sink's core side (`Sink`, `SinkTap`, `SinkAdapter`) | `ear_core` (`sink.hpp`) | L-4b | main loop; tap on the data loop | — | `NodeHost`, `PortBlock` |
| `ThreadSinks` | `ear_pipewire` (`handoffs.hpp`, `handoffs.cpp`) | L-4b | main loop; its own threads | `SinkConsumer`, `Executor` | `NodeHost` |
| The block's taps | `ear_pipewire` (`port_blocks.cpp`) | L-4b | data loop | `SinkTap::write` | the block's `process` |
| The harness | `ear_type_harness` (`lifecycle_harness.hpp`, `testing/node_harness.cpp`) | L-4a, L-4b | the test's | a real `NodeHost` over fakes | type tests, `test/nodes/` |
| Lifecycle commands, results, refusals | `ear_control` (`commands.hpp`, `lifecycle_wire.hpp`, `commands.cpp`) | L-6 | control thread (decode, encode) | — | `ControlSession`, the keeper |
| Health on the wire, `node.gone`, `applied` on `node.updated` | `ear_control` (`events.hpp`, `events.cpp`, `gateway.cpp`) | L-6 | main loop (gateway's side), control thread (encode) | — | `NodeHost`, `DesiredGraph` |
| `action` records | `ear_control` (`status_log.cpp`) | L-6 | main loop | `OutputLines` | `MainLoopSide` through `ActionListener` |
| The keeper's lifecycle step | `ear_config` (`keeper.cpp`) | L-6 | keeper thread | `MainLoopSide` (posted) | `GraphFileKeeper::submit` |
| `MainLoopSide::operate`, the reply mapping | `ear_config` (`main_loop_side.cpp`) | L-6 | main loop | `NodeOperations`, `ActionListener` | the keeper's posted step |
| The editor's minimum | `web/src/` (`protocol.ts`, `graph.ts`, `health.ts`, `components/`) | L-6 | browser | `RpcClient` | — |

Dependencies keep AGENTS.md's direction: `lifecycle.hpp` and `sink.hpp` are `ear_core`'s and see only the standard
library and `ear_core`; `ThreadSinks` is `ear_pipewire`'s; `lifecycle_wire.hpp` is `ear_control`'s and sees only
`graph_spec.hpp`; the mapping from core's `LifecycleReply` to the wire lives in `ear_config` (which sees both), so
`ear_core` names no wire code (D-L35).

### 1.2 What this page takes as given

From L-3d (its `node_host.hpp`, in flight; node-lifecycle.md 5.2, 4.3, 4.5, D-L22, D-L35), in namespace `ear`:
`NodePhase`, `NodeState` (six colours), `Wanted`, `NodeCause`, `FailureReason::Code::lost`, `WaitingFor {Kind {olderNode,
block, companion, consumer}; name}`, `NodeHealth {name, state, reason, phase, wanted, cause, waitingFor, blockLost}`,
`NodeHealthListener::hearsSuperseded()`, `LifecycleApplied {start, stop, restart, retry, unchanged}`,
`LifecycleRefusal {Code {unknownNode, nodeDisabled, notRestartable, stopping}; Reason {none, notStarted, blockLost}}`,
`LifecycleReply = std::expected<LifecycleApplied, LifecycleRefusal>`, and `NodeOperations` with `start`, `stop` and
`restart` (each `LifecycleReply (const NodeName&)`), implemented by `NodeHost`. L-3d moved the machines' names from
`ear::lifecycle` (`node_machines.hpp`) into `ear` (`node_lifecycle.hpp`), and the harness header spells
`LifecycleReply` (node-lifecycle.md revision 2.9).

From L-5 (built): `NodeSpec::disabled`, `NodeOutcome {restart, disable, enable, stored}`, `NodeChange {outcome, keys}`
(L-3d adds `portCaused`), `DesiredNodeListener::nodeChanged`, `Plan::outcomes()`, `GraphFileModel::disableNode` and
`enableNode`.

From L-7: `StallWatch` and the stall records; nothing here depends on it.

## 2. One configuration, end to end: the call sequences

The machines' tables (node-lifecycle.md 4.1 to 4.4) decide every transition; these sequences say which component calls
which, and on which thread, so an implementer sees where each interface below is used. MT is the main loop's thread,
KT the keeper's, DL PipeWire's data loop, ST a sink's thread, CT the control thread. Each `Tell` is one health change
told to every `NodeHealthListener` (`StatusLog`, then `MainLoopGateway`) before the next step.

**Create** (a `node.add`, a hand edit, or boot):

1. KT: the keeper makes a candidate and posts a step; MT: `MainLoopSide::offer` → `DesiredGraph::plan`, then `commit`.
2. MT, `commit`: `MainLoopGateway::nodeAdded` (observer: `NodeAdded`, `since` stamped, 5.4) then `NodeHost::nodeAdded`.
3. `NodeHost`: the aggregate's `add`; `Tell` `added`; the block's making queued into the turn's flush.
4. Rule 1 configures: `NodeHost` makes a `Configuration` (3.3), then, timed as the hook `configure` (3.4):
   `config->make(configuration.context)` → `SpecConfig::make` → `SpecInstance`, the type's object constructed;
   `instance->configure(InstanceContexts{node, node})` → the type's `configure(CreateOf<spec>&, settings)`, whose
   calls land on `NodeHost`'s `CreateContext` (each recorded, nothing made but sinks and recordings); `Configured`.
5. `NodeHost` checks the return (3.3's rules); `Tell` `configuring`; each sink's `Sink::begin(consumer, begun)`.
6. ST: `SinkConsumer::begin()`; posts `begun` to MT through the main-loop `Executor`.
7. The flush: `PortBlockFactory::make`. Later, DL then MT: `PortBlockEvents::ready`; the next flush makes each outside
   node (`OutsideAdapter::make`); MT: `OutsideNodeEvents::ready`.
8. Every outside node ready and every sink begun: `resourcesReady`; the code prepared (`AudioCode::prepare`); `Tell`
   `configured`.
9. Wanted running, block ready: `PortBlock::carryLatency`, then `PortBlock::drive(DriveRequest{code, faded, sinks})`;
   `Tell` `starting`.
10. DL applies it between two cycles; MT: `done(P)` → `instance->start()`, timed as `start`; `Tell` `running`.

**Stop** (`node.stop`): CT → KT (no file) → MT: `MainLoopSide::operate(stop, name)` → `NodeOperations::stop(name,
judged)` (5.6) → the aggregate judges; `judged(reply)` → `ActionListener::actionAnswered` (the `action` record); then
`instance->stop()` (timed); `PortBlock::fadeOut`; `Tell` `stopping` (cause `stop`); the reply is posted. DL acknowledges
the fade → MT: `drive(DriveRequest{nullptr})`, the taps unbound in the same invoke → `done` → the code's `release` then
`prepare` (D-L33); `Tell` `stopped`. Nothing is closed: sinks drain and wait, outside nodes run, the instance stays.

**Start** (`node.start`, on `stopped`): `NodeOperations::start` → `judged` → `drive(DriveRequest{code, faded, sinks})`,
the same taps bound again → `Tell` `starting` → reply → `done` → `instance->start()` → `Tell` `running`.

**Restart** (`node.restart` on `running`; a restart-only change is the same with cause `settings`): `judged` →
`instance->stop()` → `fadeOut` → `Tell` `stopping` (`restarting`, `action`) → reply → faded → silence → silenced →
`release`; outside nodes' destruction queued; `Sink::close` and legacy `Handoff::close`; arm the 10 s timer → every
`closed` → `instance->close(closed)` (legacy: `RunObject::close`) → MT, I5 step 5: the code, then the instance, then
sinks, recordings, context and channel freed → rule 1 configures again (Create 4 to 10, cause `action`).

**Change** (`node.update`, restart-only): KT candidate → MT `offer` → `commit` → `MainLoopGateway::nodeChanged(spec,
config, change)` (`NodeUpdated`, `applied` `restart`) → `NodeHost::nodeChanged` → the aggregate's `change` → as Restart,
cause `settings` with the changed keys. On a `stopped` node: no `stop()`, no fade (node-lifecycle.md 4.4).

**Disable** (`node.disable`): KT `GraphFileModel::disableNode` → candidate → MT `offer` → `commit`, outcome `disable` →
gateway `NodeUpdated` (`disabled`, `applied` `disable`) → `NodeHost::nodeChanged` (`disable`) → as Restart's teardown
with intent `disable` (`stop()` and the fade only when `running`) → `Tell` `stopping` (`disabling`) → the reply
`Applied{disable}` (from `Judged::outcomes`, 5.7) → … → configuration `none` → `Tell` `disabled`. Block and wires stay.

**Enable** (`node.enable`): as Disable to the commit, outcome `enable` → `NodeHost::nodeChanged` (`enable`) → rule 1
configures (Create 4 to 10) with cause `enabling`; the reply `Applied{enable}` after `Tell` `configuring`.

**Remove** (`node.remove`): commit → gateway `NodeRemoved` → `NodeHost::nodeRemoved` → intent `remove` → `Tell`
`stopping` (`removing`) → (if driven) `stop()`, fade, silence → teardown → the block's destruction in the flush →
`NodeHealthListener::nodeGone(name, dropped)` → `StatusLog`'s `removed` record; the gateway's `NodeStopped` then
`NodeGone` (the client, which dropped the node at `node.removed`, ignores both).

**Failure** (a card lost while `running`): MT `OutsideNodeEvents::lost(detail)` → `NodeHost` input → the aggregate's
`fault(engineRefused)` → `instance->stop()` (it had started) → `fadeOut` → `Tell` `failed` → silence → teardown as
Restart's → configuration `none`; the block, its wires and its meters stay. A sink consumer that cannot begin: ST
`handle.lost(detail)` posts before `begun` → MT `fault(startFailed)` (not rested) → teardown with no barrier (never
driven) → `Sink::close` runs `finish` (begin ran) → `Tell` `failed`.

## 3. L-4a: the type's class

**Purpose.** A type is one class with three hooks and no `NodeConfig`, `StartResult`, recipe or run object to write;
the framework makes one instance per configuration and calls its hooks only where the machines say. This step adds the
contexts the class is handed, the erased instance `NodeHost` drives, the harness over the real `NodeHost`, and moves
`white`, `card` and `rtp` (and the slice 2 tests' spec types) onto it; `sendspin` and hand-made `NodeTypeOf` types keep
the old SPI through the legacy adapter (node-lifecycle.md 5.10).

### 3.1 `lifecycle.hpp`: the type author's view

The header is the declaration's text; this table gives what the header's comments cannot hold in one line.

| Declaration | Who makes it, who holds it | Thread | Errors |
|---|---|---|---|
| `using NodeCode = RunCode;` | — | — | L-4a renames the class (3.5); the alias then lives in `node_type.hpp` the other way round until 3c |
| `Configured::applied()`, `failed(detail)`, `isFailed()`, `detail()` | the type returns it by value | main loop | none; `failed` is a node failure (`start-failed: <detail>`), never a throw |
| `Changed` | declared for 3d; no 3a code makes one | — | its statics are undefined until 3d, so a call does not link |
| `NodeContext` (abstract) | `NodeHost`, one per configuration (3.3) | main loop; `handle()`'s result any of the node's threads | none |
| `SinkRequest` (abstract) | `NodeHost`, one per `cx.sink` call, owned by the configuration | main loop, inside `configure` | `consumedBy` twice or with null: `std::logic_error` |
| `CreateContext` (abstract) | `NodeHost` (its `Node`), valid only inside `configure` | main loop | a call outside `configure`, a second `use`, a bank direction a value contradicts: `std::logic_error` naming the node and the call |
| `NodeOf<Spec>` | `SpecInstance`, a member before the type's object; the type keeps `NodeOf<spec>&` | main loop (but `handle()`) | none |
| `CreateOf<Spec>` | `SpecInstance`, on its stack for one `configure` call; never copied | main loop | compile time: a bank of another spec, of the wrong reach or direction; run time: `use` of a code written against another spec (`std::logic_error`) |
| `ChangeOf<Spec>` | declared in 3a; a class may write the change version, and 3a never calls it | — | every change restarts the node in 3a whatever the class writes (D-M2, the owner's Q1) |
| `InstanceContexts` | `NodeHost`, on its stack for one `configure` | main loop | — |
| `ErasedInstance` (abstract) | `NodeConfig::make` returns it in a `std::unique_ptr`; the `Configuration` owns it | main loop | a type's exception passes through every call; `NodeHost` catches it there (3.3) |
| `spec_detail::SpecInstance<Spec, Type>` | `SpecConfig::make` | main loop | the class's checks are `static_assert`s (3.10) |

`ErasedInstance` differs from node-lifecycle.md 5.9's sketch in two members (D-M6): `ready` returns
`std::expected<void, FailureReason>` (a legacy recipe can fail, and 5.10 makes that a fault; a `void` could not say it),
and `hasRecipe()` lets `NodeHost` keep D-L40's check (a legacy recipe beside an outside node) now that the recipe sits
inside the instance.

### 3.2 What a type may call, and when (the rules a compiler cannot check)

Each is node-framework.md 6.10's rule table as 3a has it. Each broken rule fails the node `start-failed: <text>` and
never leaves `NodeHost` (D-M3): the context throws `std::logic_error` at the call, which escapes the type's `configure`
and is caught as the hook's throw (`configure threw: <what>`); a rule checked at the return is turned into the same
failure by `NodeHost`, with the text below.

| Rule | Checked | The failure's detail |
|---|---|---|
| `use` exactly once | a second at the call; none at an applied return | `configure threw: use called twice`; `configure gave no code` |
| Each sink has exactly one consumer | a second or null at the call; none at an applied return | `configure threw: consumedBy called twice on sink <bank>`; `configure gave sink <bank> no consumer` |
| A context is used only inside its `configure` | at the call | `<hook> threw: <call> outside configure` (a type that kept `cx` and calls it from `start()`) |
| A code written against another spec | at `use` | `configure threw: a type's configure gave a code written against another type's spec` |
| A sound card's direction agrees with its bank's | at the call | `configure threw: a playback card joins an inside output bank` (or capture, input) |
| `declareLatency` once per output bank | at the call | `configure threw: latency declared twice for bank <bank>` |
| The return checks run only on `applied` | — | a `failed` answer is never also a broken rule (node-lifecycle.md 5.9's edges) |

### 3.3 `NodeHost`'s side of the contexts

`NodeHost::Impl::Node` (today `final : PortBlockEvents, StartContext, Ready`) becomes `final : PortBlockEvents,
StartContext, Ready, CreateContext`, so the two contexts share one set of internals (node-lifecycle.md 5.9, "n3"):
`format()` and `node()` are one override each for both bases; `soundCard`, `rtpReceiver` and `acquire(SoundCardRequest)`
call one `askOutside`; `openRecording` and `acquire(RecordingRequest)` one `openRecording`; `declareLatency(from, to, l)`
and the legacy `declareLatency(bank, l)` one record in `latency`. Each checks `configuring_` (the generation inside its
`configure`, else none) first and throws `std::logic_error("<node>: <call> outside configure")`.

The `Configuration` (node-lifecycle.md 5.1) gains these members, in declaration order, which is reverse teardown
order (I5 step 5 frees from the bottom up):

```cpp
struct Configuration {
	std::uint64_t generation;
	std::shared_ptr<const NodeConfig> config;
	std::shared_ptr<Channel> channel;                          // InsidesEvents for NodeOf::handle() (3a) and legacy
	class Context final : public NodeContext { /* format, node name, *channel */ } context;   // new (L-4a)
	std::vector<std::unique_ptr<RecordingFile>> recordings;    // outlive every sink consumer that writes them
	std::vector<std::unique_ptr<SinkSlot>> sinks;              // new (L-4b): 4.2; by pointer, so a SinkRequest& the
	                                                           // type holds survives a later cx.sink
	std::unique_ptr<ErasedInstance> instance;                  // new (L-4a); the legacy recipe and run object inside it
	std::unique_ptr<AudioCode> code;                           // the code NodeCode resolved to (own, or PassThrough)
	std::vector<std::unique_ptr<Handoff>> handoffs;            // legacy only
	// ... today's outside requests, outside nodes, timers, faded flags, pairs, latency, flags; `recipe` and `object`
	// leave Configuration (they are LegacyInstance's)
	bool codeGiven = false;                                    // use() was called
};
// And on Node, for its life (3.4): the hooks already reported slow.
std::set<std::string_view> hooksReportedSlow;
```

The `configure` step (Create 4 and 5 of section 2), as `NodeHost` carries out the machine's `configure` action:

1. Make the `Configuration` (new generation, the queued config, a new `Channel`, its `Context`).
2. Under `MainLoopActivity::Scope("configure", node)` and the hook timer: `instance = config->make(context)`, then
   `Configured answer = instance->configure(InstanceContexts{*this, *this})`, with `configuring_` set around both. A
   throw from either (`std::exception`: its `what()`; anything else: `unknown exception`) is the fault
   `<hook> threw: <what>` with `hook` = `configure`, by 4.2's code by fact.
3. `answer.isFailed()`: the fault `start-failed: answer.detail()`. Else the return checks of 3.2.
4. Legacy (`instance->hasRecipe()` with an outside node asked): the fault `start-failed` with D-L40's text (D-L40 as
   built threw out of `NodeHost`; superseded by D-M3 in L-4a's review).
5. Each sink with a consumer: `sink.sink->begin(std::move(sink.consumer), begunCall(generation, index))`.
6. The machine hears its answer (`resourcesReady` in the same turn when nothing is pending).

At T3 (every resource ready) a legacy instance's `ready(Ready&)` runs first (as built: before the machine hears it),
then `code->prepare`. At T5's completion `instance->start()` runs, timed as `start`; at T6, T11, T14, T17, T20
`instance->stop()` when `started`, timed as `stop`; a `stop()` that throws is node-lifecycle.md 5.9's edge (a held fault
during a stop; a `log` record from source `ear`, level `error`, `stop() threw: <what>`, during a teardown). At I5 step 4
`instance->close(closedCall)` (the new SPI's default calls it at once).

`NodeCode` resolution, at `use`: an own `AudioCode` is kept as `code`; a pass-through is kept as `pairs` and resolved
into today's `PassThrough` (`pass_through.hpp`) at the return checks, against the node's ports (a pair naming a bank
the node lacks, of the wrong direction or another size: `configure gave a pass-through ...`, by 3.2's rule).

### 3.4 Hooks timed: `ear.hook-slow`

`NodeHost` times `configure` (the instance's making and its `configure` together; a legacy `start` counts as
`configure`), `start` and `stop` with `Timers::now()` (D-L12). A call past `hookSlowBound` is reported once per node and
hook for the `Node`'s life (a restart does not report again; a name added back is a new `Node`):

```cpp
// node_host.hpp (L-4a), beside the other bounds
/// A hook that ran longer than this on the main loop is ear.hook-slow (node-framework.md 6.4, I11; control-plane.md 4.6).
inline constexpr std::chrono::milliseconds hookSlowBound{20};
```

Told through today's `NodeDiagnosticListener::nodeDiagnostic(node, "ear.hook-slow", detail, fields)` with `detail`
`"<hook> ran <ms> ms"` and `fields` `{{"ms", ms}}` (D-M7: the record's `subject` arrives with slice 5's diagnostics
model; until then the hook is named in the detail). `StatusLog`'s record, and `output-records.json`'s new case:

```json
{"v":1,"seq":7,"t":"2026-11-09T10:00:00.000000Z","kind":"diagnostic","node":"rec","code":"ear.hook-slow","ms":31,"msg":"node rec ear.hook-slow ms=31: configure ran 31 ms"}
```

A legacy recipe is not a hook (its activity is still `recipe`), and `prepare` and `release` are the code's, not hooks.

### 3.5 `node_type.hpp`: old and new

| Old | New (L-4a) |
|---|---|
| `class RunCode { ... };` and `inline RunCode passThrough(...)` | `class NodeCode { ... };` (the same members), `inline NodeCode passThrough(...)`, and `using RunCode = NodeCode; // until slice 3c (sendspin's start)`; `lifecycle.hpp`'s `using NodeCode = RunCode;` goes |
| `StartResult(RunCode code, RunRecipe recipe = nullptr)` | `StartResult(NodeCode code, RunRecipe recipe = nullptr)` (the alias keeps sendspin compiling) |
| `class NodeConfig { virtual const Ports& ports() const = 0; virtual StartResult start(StartContext&) const = 0; };` | `class NodeConfig { virtual const Ports& ports() const = 0; virtual StartResult start(StartContext& context) const; /* default: throws std::logic_error("a type on the new SPI has no start") */ virtual std::unique_ptr<ErasedInstance> make(NodeContext& node) const; /* default: a LegacyInstance over start (node-lifecycle.md 5.10) */ };` with `class ErasedInstance; class NodeContext;` forward-declared |
| `struct SoundCardRequest { CardId card; CardDirection direction; std::string bank; };` (`sound_card.hpp`) | `std::string bank{};` (D-M15: a type's designated initializer leaves it out, and GCC's `-Wmissing-field-initializers` stays quiet); the same for `RtpReceiverRequest::bank` (`rtp_receiver.hpp`) |

The private adapter, in `src/core/legacy_instance.hpp` and `.cpp` (not public: only `NodeConfig::make`'s default and
`NodeHost` use it):

```cpp
namespace ear::detail {
/// The old SPI under the new machines (node-lifecycle.md 5.10): sendspin's NodeTypeOf and hand-made test types.
class LegacyInstance final : public ErasedInstance {
public:
	explicit LegacyInstance(const NodeConfig& config) noexcept;                 // config outlives it (I5 step 5)
	Configured configure(InstanceContexts& cx) override;   // config.start(cx.legacy); failed -> Configured::failed;
	                                                       // else cx.create.use(code), keeps the recipe
	bool hasRecipe() const noexcept override;
	std::expected<void, FailureReason> ready(Ready& ready) override;          // runs the recipe, keeps the object
	void close(std::move_only_function<void()> closed) override;              // RunObject::close, or closed() at once
private:
	const NodeConfig& config_;
	RunRecipe recipe_;
	std::unique_ptr<RunObject> object_;   // destroyed with the instance, after the code (I5 step 5)
};
}
```

### 3.6 `type_spec.hpp` and `lifecycle.hpp`: where `makeNodeType` goes

`type_spec.hpp` keeps the spec's vocabulary (bank handles, `Banks`, `TypeSpec`, `CheckedOf`, `Members`, `Io`,
`bankPair`, `ErasedSpecCode`, `eraseSpecCode`). L-4a moves `SpecConfig`, `SpecNodeType` and `makeNodeType` from it to
the end of `lifecycle.hpp` (D-M1), since they now make an instance, and a type's `<type>_spec.hpp` includes
`lifecycle.hpp`. Their new shape:

```cpp
namespace spec_detail {
template <const auto& Spec, class Type> class SpecConfig final : public NodeConfig {
public:
	explicit SpecConfig(const CheckedOf<Spec>& settings)
		: settings_(settings), ports_(Type::ports(settings_).ports()) {}
	const Ports& ports() const override { return ports_; }
	std::unique_ptr<ErasedInstance> make(NodeContext& node) const override {
		return std::make_unique<SpecInstance<Spec, Type>>(node, settings_);
	}
	// no start: NodeConfig's default throws, and nothing calls it for a SpecConfig
private:
	CheckedOf<Spec> settings_;
	Ports ports_;
};
/// As today, but its static_assert on a static start goes: SpecInstance holds the class's checks (instantiated here,
/// so a type that breaks one does not compile at makeNodeType).
template <const auto& Spec, class Type> class SpecNodeType final : public NodeType {
	static_assert(sizeof(SpecInstance<Spec, Type>) > 0);   // forces the class's checks at makeNodeType
	// ... the linkage and folder-name checks and makeConfig with `check`, unchanged
};
}
template <const auto& Spec, class Type> std::unique_ptr<const NodeType> makeNodeType();   // unchanged signature
```

`type_spec.hpp`'s header comment's `white` example becomes the class (lifecycle.hpp's own comment shows it). The
`ProcessOf` and `BankBoundCode` machinery stays in `type_spec.hpp`; `CreateOf::use` calls it (the check and the binding
that `SpecConfig::start` made before move there).

### 3.7 Deleted in L-4a

- `src/core/include/ear/core/run_requests.hpp` and `src/core/run_requests.cpp`: `outsideSuffix` and `requestedBank`
  become private functions of `node_host.cpp`, since the harness now runs `NodeHost` itself (node-lifecycle.md 11.3).
- `SpecConfig::start`, `SpecNodeType`'s `static_assert` on `Behaviour::start`, and every `static StartResult
  start(StartContext&, const CheckedOf<spec>&)` in `white`, `card`, `rtp` and the core tests' spec types.
- `node_harness.hpp`'s and `type_harness.hpp`'s slice 2 classes, replaced by 3.8's (which move into those two files,
  `lifecycle_harness.hpp` deleted).

### 3.8 The harness

`lifecycle_harness.hpp` declares the harness whole (node-framework.md 7.6's class as 3a has it). It lives in
`ear::testing::next` only because today's harnesses hold the names `NodeHarness` and `TypeHarness`, which today's tests
use; L-4a moves the two classes into `node_harness.hpp` and `type_harness.hpp` (namespace `ear::testing`), deletes
`lifecycle_harness.hpp`, and rewrites every caller. Differences from 7.6, each because 3a lacks the thing: no `feed`,
`held`, `runLoops` (3b, 3c, 3d); `state()` and `reason()` read through `health()`; `change` returns `NodeOutcome`.

`testing/node_harness.cpp` builds, per `NodeHarness`, one `NodeHost` over these fakes (each private to the file):

| Fake | Implements | Behaviour |
|---|---|---|
| `ManualExecutor` | `Executor` | queues posts; every harness call pumps it until empty (and pumps again after anything it ran posted) |
| `FakeClock` | `Timers` | `now()` from a counter; `after` keeps a sorted list; `advance(by)` moves the counter and runs each due timer, pumping between |
| `FakeBlocks`, `FakeBlock` | `PortBlockFactory`, `PortBlock` | one block per `make`; `drive` and `fadeOut` queue requests; with `autoComplete` they complete in the pump, else on `acknowledgeFade()` and `completeBarrier()`; `settle()` runs completed ones; `process` NaN-fills outputs, runs the driven code with a `GraphCycle` it advances, applies the fade gain per faded output (recording each port's last gain), then each `SinkBinding`'s tap; `blockId()` returns a fixed id |
| `FakeOutside<Request>` | `SoundCardAdapter`, `RtpReceiverAdapter` | records a `CompanionFake` per `make`; `ready(bank)` and `lose(bank, detail)` tell its events |
| `FakeHandoffs` | `HandoffAdapter` | legacy: hands each write straight to the consumer; `close` finishes and posts `closed` |
| `FakeSinks` (L-4b) | `SinkAdapter`, `Sink`, `SinkTap` | `begin` calls `consumer->begin()` at once on the test's thread, then posts `begun`; the tap copies each chunk into the `CompanionFake` and calls `consume` at once; `close` calls `finish` if it began and posts `closed(0)` |
| `Listeners` | `NodeHealthListener`, `NodeNoteListener`, `NodeDiagnosticListener`, `EngineDiagnosticListener` | capture health (as last told), notes, diagnostics, `nodeGone` |

What a test reads: `health()`, `phase()`, `gone()`, `notes()`, `diagnostics()`, buffers by port, `lastFadeGain`, the
companions with their requests, readiness and chunks. `stop()`'s pairing with `start()` is not observable through a
type the harness did not write; it is tested in `node_host_test.cpp` with a probe class whose hooks record themselves
(D-M14). The all-type test (`test/nodes/harness_test.cpp`) walks every type but `notYetInHarness` through add,
`blockReady`, readiness of every companion, `process` (every output sample of every cycle written), stop, start,
restart, disable, enable and remove, checking phases and samples.

### 3.9 `white`, `card` and `rtp`, written whole

Each `<type>.cpp` keeps its one function, `makeNodeType<<type>::spec, <type>::<Class>>()`; each spec header includes
`ear/core/lifecycle.hpp` in place of `type_spec.hpp`. Settings rows and banks are unchanged but where shown.

```cpp
// src/nodes/white/white_spec.hpp
namespace ear::nodes::white {
inline constexpr auto noise = bank::output<"noise">;
inline constexpr TypeSpec spec{.settings = SettingsTable<>{"white"}, .banks = Banks<noise>{}};
using Checked = CheckedOf<spec>;
struct WhiteCode { /* unchanged: process(Io<spec>, const GraphCycle&) noexcept and next() */ };
struct White {
	static Members<spec> ports(const Checked&) { return {noise(members("mono"))}; }
	Configured configure(CreateOf<spec>& cx, const Checked&) {
		cx.use(WhiteCode{});
		return Configured::applied();
	}
};
}
```

```cpp
// src/nodes/card/card_spec.hpp (rows, banks, spec and Checked unchanged)
struct Card {
	static Members<spec> ports(const Checked& s) {
		return {play(members(s.get<playbackRow>())), capture(members(s.get<captureRow>())),
		        fromCard(members(s.get<captureRow>())), toCard(members(s.get<playbackRow>()))};
	}
	static std::expected<void, std::string> check(const Checked& s);   // unchanged
	Configured configure(CreateOf<spec>& cx, const Checked& s) {
		if (s.get<playbackRow>() > 0)
			cx.soundCard(toCard, SoundCardRequest{.card = s.get<cardRow>(), .direction = CardDirection::playback});
		if (s.get<captureRow>() > 0)
			cx.soundCard(fromCard, SoundCardRequest{.card = s.get<cardRow>(), .direction = CardDirection::capture});
		cx.use(passThrough({bankPair(play, toCard), bankPair(fromCard, capture)}));
		return Configured::applied();
	}
};
```

`to_card` is faded with the public outputs (D-L16): `NodeHost` marks a playback card's bank faded in `soundCard`.

```cpp
// src/nodes/rtp/rtp_spec.hpp (rows, banks, spec and Checked unchanged)
struct Rtp {
	static Members<spec> ports(const Checked& s) {
		return {stream(members(s.get<channelsRow>())), fromRtp(members(s.get<channelsRow>()))};
	}
	Configured configure(CreateOf<spec>& cx, const Checked& s) {
		cx.rtpReceiver(fromRtp, RtpReceiverRequest{.address = s.get<addressRow>(),
		                                           .port = static_cast<std::uint16_t>(s.get<portRow>()),
		                                           .encoding = s.get<formatRow>(),
		                                           .rate = s.get<rateRow>().value_or(cx.format().rate),
		                                           .latencyMs = s.get<latencyRow>()});
		// The receive buffer holds the stream that long before it leaves; PipeWire's receiver reports none of it.
		const std::uint32_t buffered = s.get<latencyRow>() * cx.format().rate / 1000;
		cx.declareLatency(fromRtp, stream, Latency{buffered, buffered});   // on stream, as today (D-L17)
		cx.use(passThrough({bankPair(fromRtp, stream)}));
		return Configured::applied();
	}
};
```

### 3.10 Compile-time refusals (`type_spec_bad.cpp`'s new cases)

Each is a CTest case in `src/core/CMakeLists.txt` with this regex (GCC 14's wording; the first five are the messages
`lifecycle.hpp` gives). The cases that today test `Behaviour::start` (`behaviour-without-ports` and the control's
`Good` type) are rewritten to the class.

| Case | What it writes | Regex |
|---|---|---|
| `class-without-ports` | no static `ports` | `a type's class has static Members<spec> ports` |
| `configure-wrong-signature` | `Configured configure(CreateOf<spec>&, Checked)` (by value) | `a type's class has Configured configure` |
| `configure-not-a-member` | `static Configured configure(CreateOf<spec>&, const Checked&)` | `a type's class has Configured configure` |
| `start-wrong-signature` | `int start()` | `a type's start is void start\(\)` |
| `stop-wrong-signature` | `void stop(int)` | `a type's stop is void stop\(\)` |
| `constructor-neither` | `explicit Type(int)` only | `a type's class is constructed from NodeOf<spec>& or from nothing` |
| `legacy-start-on-spec-type` | `static StartResult start(StartContext&, const Checked&)` | `writes configure, not a static start` |
| `change-version-wrong-signature` | `Changed configure(ChangeOf<spec>&, Checked, Checked)` (D-M2 as amended: the right shape compiles, and the control writes it) | `a type's change version is Changed configure` |
| `create-has-no-hold` | `cx.hold(out)` | `has no member named 'hold'` |
| `create-has-no-when-ready` | `cx.whenReady(...)` | `has no member named 'whenReady'` |
| `create-has-no-unclaim` | `cx.unclaim(...)` | `has no member named 'unclaim'` |
| `configured-has-no-restart` | `Configured::restart("x")` | `'restart' is not a member of 'ear::Configured'` |
| `sound-card-on-public-bank` | `cx.soundCard(out, ...)` | `a sound card joins an inside bank of the spec` |
| `rtp-receiver-on-output-bank` | `cx.rtpReceiver(insideOut, ...)` | `an RTP receiver joins an inside input bank of the spec` |
| `sink-on-input-bank` | `cx.sink(in, ...)` | `a sink reads an inside output bank of the spec` |
| `declare-latency-bank-the-spec-lacks` | `cx.declareLatency(in, otherOut, ...)` | `declareLatency takes its spec's banks` |
| `declare-latency-backwards` | `cx.declareLatency(out, in, ...)` | `declareLatency names a pair: an input bank, then an output bank` |
| `context-bank-the-spec-lacks` | `cx.sink(otherInsideOut, ...)` | `a context takes only its spec's banks` |

### 3.11 Tests this step adds or rewrites

node-lifecycle.md 11.1 to 11.3 name the cases; at module level: `type_spec_test.cpp` (the class's constructor found,
hooks called, a spec code bound by `use`, another spec's code failing the node); `node_host_test.cpp` (the probe class
for `stop()`'s pairing, `ear.hook-slow` with the fake clock advanced inside a hook, every rule of 3.2, `configure`
throwing after `openRecording`); the three types' tests on `TypeHarness`; `output-records.json`'s `ear.hook-slow` case.

## 4. L-4b: the interim sink

**Purpose.** Audio leaves the realtime thread through framework code, not the type's: the block writes a sink's tap
after the code, from an inside output bank, and the sink's own thread hands stamped chunks to the type's consumer. This
step adds the sink, wires it through `NodeHost`, the block and `ear_pipewire`, and moves `file` onto it, so `Recorder`
and `FileRun` go (node-lifecycle.md 5.11; 3b moves the realtime side behind a ring companion with the same API).

### 4.1 `sink.hpp`

The header is the declaration's text (D-M4 says why the sink has its own header). What it adds to node-lifecycle.md
5.11: `SinkBinding` (what a drive carries per sink: the tap and the bank's place among the output spans), `Sink`'s three
calls, and the exact moment each runs:

| Call | Thread | Owner after | Errors |
|---|---|---|---|
| `SinkAdapter::make(node, bank, channels, frames)` | main loop, inside `CreateOf::sink` | the configuration (`SinkSlot`) | `std::bad_alloc`, `std::system_error` (thread): through `configure`, so `configure threw: ...` (D-M17) |
| `Sink::tap()` | main loop | — | none |
| `Sink::begin(consumer, begun)` | main loop, at `configure`'s applied return | the sink owns the consumer | none; the consumer reports by `handle.lost` |
| `SinkConsumer::begin()`, `consume()`, `finish()` | the sink's thread | — | must not throw (`std::terminate`) |
| `SinkTap::write(bank, frames, stamp)` | data loop, from the block's `process` after the code | — | `noexcept`; drops counted |
| `Sink::close(closed)` | main loop, at I5 step 3 | — | none; `closed(dropped)` on the main loop |
| `~Sink()` | main loop, after `closed`, or never begun nor closed | — | — |

### 4.2 Core changes: old and new

| Header | Old | New (L-4b) |
|---|---|---|
| `node_host.hpp` | `struct DriveRequest { AudioCode* code = nullptr; std::span<const bool> faded; };` | `struct DriveRequest { AudioCode* code = nullptr; std::span<const bool> faded; std::span<const SinkBinding> sinks{}; };` with: "the block writes each binding's tap after the code, every cycle it runs that code, and unbinds them all in the invoke that applies the next request" |
| `node_host.hpp` | `ResourceRegistry(SoundCardAdapter&, RtpReceiverAdapter&, HandoffAdapter&, RecordingAdapter&)` and the three-argument form | `ResourceRegistry(SoundCardAdapter&, RtpReceiverAdapter&, HandoffAdapter&, SinkAdapter&, RecordingAdapter&)` and `(..., HandoffAdapter&, SinkAdapter&)`; `SinkAdapter& sinks() const noexcept` |
| `node_host.hpp` | `#include` list | adds `"ear/core/sink.hpp"` |
| `node_host.hpp` | `NodeHealthListener::nodeGone`'s comment: "frames its handoffs refused" | "frames its sinks and legacy handoffs refused" |

`NodeHost`'s private `SinkSlot` (in `node_host.cpp`):

```cpp
struct SinkSlot final : SinkRequest {                     // one cx.sink call
	std::string bank;
	std::unique_ptr<Sink> sink;
	std::unique_ptr<SinkConsumer> consumer;              // until Sink::begin takes it
	SinkBinding binding;                                 // tap and the bank's first inside output and size
	bool begun = false;
	bool closed = false;
	void consumedBy(std::unique_ptr<SinkConsumer> c) override;   // once, not null (3.2)
};
```

`NodeHost` builds the configuration's `std::vector<SinkBinding>` once, at the return checks, and every drive of that
configuration points `DriveRequest::sinks` at it; a silence carries none. A sink's bank is never faded (5.1). Readiness
4': the configuration is `prepared` only once every slot is `begun` (`waitingFor` `consumer` until then). Teardown
step 3: `close` on every slot, its `closed` adding `dropped` to the node's count (today's handoff path), and step 4
waits for every slot and legacy handoff. `NodeHealthListener::nodeGone`'s count is the sum.

### 4.3 `ThreadSinks` (`ear_pipewire`)

Declared in `handoffs.hpp` beside `ThreadHandoffs` (its constructor takes the main-loop `Executor`, not a `pw_loop*`,
so `begun` and `closed` share the FIFO the node's `InsidesEvents` post through: D-L14). Its private `ThreadSink final :
Sink, SinkTap` reuses `ThreadHandoff`'s `FrameRing`, wake and `pw_thread_loop` (`handoffs.cpp:21-112`), and adds a
stamp ring of `struct Stamped { ChunkStamp stamp; std::uint32_t frames; }` (`max(64, capacityFrames / 32)` entries:
a 2 s ring at 48 kHz holds 3000 stamps, about 16 times what its 256-frame cycles fill, so only cycles under 32 frames
can run it out first, and those drop whole chunks, counted), the consumer, a `began` flag read only on its thread, and the drain that pairs stamps
with frames and calls `consume` with a `Chunk` (wrapping at the ring's end in two calls, node-lifecycle.md 5.11).
`main.cpp` makes one `ThreadSinks sinks(mainLoopExecutor)` and passes it to `ResourceRegistry`.

### 4.4 The block's taps (`ear_pipewire`, `port_blocks.cpp`)

`FilterPortBlock` keeps the applied request's `sinks` beside its `code` (copied into its realtime state inside the
invoke that applies the request, so a tap and its code change in one step: D-L29). In `process`, after the code (or the
silence) and before metering, for each binding it builds the span list from the output spans `[firstOutput,
firstOutput + channels)` and calls `tap->write(bank, frames, ChunkStamp{cycle.position, cycle.startNs,
cycle.startRawNs, cycle.discontinuity})`. The span list lives in a per-block scratch array sized at make (the bank's
spans are already in the block's output list, so nothing allocates on the data loop).

### 4.5 `file`, written whole

```cpp
// src/nodes/file/file_spec.hpp
namespace ear::nodes::file {
inline std::optional<std::string> nonEmpty(std::string_view value);      // unchanged
inline constexpr auto pathRow = setting::required("path", setting::text<nonEmpty>("a non-empty string"));
inline constexpr auto record = bank::input<"record">;
inline constexpr auto writer = bank::insideOutput<"writer">;             // new: the sink's bank
inline constexpr TypeSpec spec{.settings = SettingsTable<pathRow>{"file"}, .banks = Banks<record, writer>{}};
using Checked = CheckedOf<spec>;
inline constexpr std::uint32_t ringSeconds = 2;

class File {
public:
	explicit File(NodeOf<spec>& node) : node_(node) {}
	static Members<spec> ports(const Checked&) {
		return {record(members("left", "right")), writer(members("left", "right"))};
	}
	static std::expected<void, std::string> check(const Checked& s);   // unchanged
	Configured configure(CreateOf<spec>& cx, const Checked& s);      // file.cpp, the only file that sees libsndfile

private:
	NodeOf<spec>& node_;
};
}
```

```cpp
// src/nodes/file/file.cpp
namespace ear::nodes::file {
namespace {
/// The sink's consumer: opens the WAV on the sink's thread (D-L18), writes each chunk, finishes the header.
class WavWriter final : public SinkConsumer {
public:
	WavWriter(int descriptor, std::string path, AudioFormat format, InsidesEvents& events)
		: descriptor_(descriptor), path_(std::move(path)), format_(format), events_(events) {}
	~WavWriter() override { close(); }
	void begin() override {
		SF_INFO info{};
		info.samplerate = static_cast<int>(format_.rate);
		info.channels = 2;
		info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
		file_ = sf_open_fd(descriptor_, SFM_WRITE, &info, SF_FALSE);
		if (file_ == nullptr) events_.lost("cannot open " + path_ + ": " + sf_strerror(nullptr));
	}
	void consume(const Chunk& chunk) override {
		if (file_ != nullptr) sf_writef_float(file_, chunk.interleaved.data(), chunk.frames);
	}
	void finish() override { close(); }

private:
	void close() noexcept {
		if (file_ != nullptr) sf_close(file_);
		file_ = nullptr;
	}
	int descriptor_;          // the framework's recording, closed after the sink (I5 step 5)
	std::string path_;
	AudioFormat format_;
	InsidesEvents& events_;   // the configuration's channel, which outlives the sink
	SNDFILE* file_ = nullptr;
};
} // namespace

Configured File::configure(CreateOf<spec>& cx, const Checked& s) {
	const std::string path = s.get<pathRow>();
	const auto descriptor = cx.openRecording(RecordingRequest{path});   // 3a interim: synchronous
	if (!descriptor) return Configured::failed("cannot open " + path + ": " + descriptor.error());
	cx.sink(writer, SinkShape{.frames = ringSeconds * cx.format().rate})
		.consumedBy(std::make_unique<WavWriter>(*descriptor, path, cx.format(), node_.handle()));
	cx.use(passThrough({bankPair(record, writer)}));
	return Configured::applied();
}
} // namespace ear::nodes::file
```

`Recorder`, `FileRun`, `channelCount` and the handoff go. `file.hpp`'s comment says the WAV is opened on the sink's
thread and a libsndfile refusal fails the node `start-failed` before it is configured.

### 4.6 Sequences

- **Begin**: `configure` returns applied → `Sink::begin` → ST `WavWriter::begin` (`sf_open_fd`, the header) → `begun`
  → MT: the slot `begun`; with every outside node ready (none for `file`), `resourcesReady`.
- **A chunk**: DL `process` → the pass-through copies `record` to `writer` → `tap->write` (frames into the ring, one
  stamp) → wake → ST pairs the stamp with its frames → `consume(Chunk)`.
- **Stop then start**: the silence's invoke unbinds the tap (no frame after the stop's position); ST drains what the
  ring held; the start's drive binds it again (frames from the start's position, the gap visible in `position`).
- **Restart** (a new `path`): the silence unbinds; `Sink::close` → ST drains, `finish` (the old WAV complete) →
  `closed(dropped)` → step 5 frees the sink, then the recording; the new configuration opens the new path and begins.
- **A WAV libsndfile cannot open**: ST `begin` → `events.lost(...)` posted, then `begun` posted → MT: the fault
  (`start-failed`, not rested) tears down with no barrier → `close` → `finish` (begin ran; `file_` null) → `closed`.

### 4.7 Tests

`sink_test.cpp` (new in `ear_pipewire_tests`, from `handoff_test.cpp`'s five, which stay for the legacy handoff): order,
drops counted with the stamp ring and the frame ring dropping together, close drains then finishes, finish only after
begin, no allocation on the tap, begin before the first consume, `begun` after a `lost` posted inside `begin`, and the
ramp through a block's sink across a stop and a start under an invoke storm (node-framework.md 9.6's [U]). The harness's
`FakeSinks`; `file_test.cpp` on `TypeHarness` (node-lifecycle.md 11.3's four cases); `white_to_file_test.cpp` unchanged
and green (exact samples, `dropped=0`).

## 5. L-6: the wire

**Purpose.** The lifecycle reaches clients and scripts: health's new members on every `node.state`, snapshot and
`node.added`, the five lifecycle requests, `node.gone`, the protocol frame, the two new refusals and the `action`
records, with the editor's minimum. control-plane.md 4.2, 4.3, 4.5, 4.8, 7.6, 9 and 9.1 are the home of every shape;
this section is the C++ that makes each and the order components call each other in.

### 5.1 `commands.hpp`: old and new

```cpp
// Old
using Command =
	std::variant<GetGraph, AddNode, UpdateNode, RemoveNode, AddRoute, RemoveRoute, GetGraphFile, SetGraphFile>;
using CommandResult = std::variant<GraphView, std::expected<void, CommandError>, GraphFile, FileRefusal>;

// New (L-6); commands.hpp includes "ear/control/lifecycle_wire.hpp"
struct StartNode { NodeName node; };     // node.start   {"name"}
struct StopNode { NodeName node; };      // node.stop    {"name"}
struct RestartNode { NodeName node; };   // node.restart {"name"}
struct DisableNode { NodeName node; };   // node.disable {"name"}
struct EnableNode { NodeName node; };    // node.enable  {"name"}
using Command = std::variant<GetGraph, AddNode, UpdateNode, RemoveNode, AddRoute, RemoveRoute, GetGraphFile,
                             SetGraphFile, StartNode, StopNode, RestartNode, DisableNode, EnableNode>;
using CommandResult = std::variant<GraphView, std::expected<void, CommandError>, GraphFile, FileRefusal, Applied,
                                   ControlRefusal>;
/// The wire's frame (control-plane.md 4.1): graph.get's "protocol" and "features".
inline constexpr int wireProtocol = 1;
inline constexpr std::array<std::string_view, 1> wireFeatures{"lifecycle"};
```

- `decodeCommand`: the five methods take `{"name"}` (a required name, `requireObject` refusing any other key);
  `node.add`'s params gain an optional boolean `"disabled"` (`decodeNodeSpec` reads it into `NodeSpec::disabled`; a
  non-boolean is `-32602` at `/disabled`). Every `-32602` gains `data.pointer` (control-plane.md 4.1, `errors.json`'s
  unknown-member case).
- `encodeCommandResult`: `Applied` → `{"applied": appliedName(kind)}`; `ControlRefusal` → `RpcError{commandRefused,
  describe(r), {"code": controlRefusalCode(r)}}` plus `"reason": reasonName(...)` for `NotRestartable`.
- `encodeGraph`: `"protocol": wireProtocol`, `"features": wireFeatures`, and per node 5.3's members.
- `lifecycle_wire.hpp`'s functions are defined in `commands.cpp` (the one home of their strings).

### 5.2 `graph_view.hpp`, `events.hpp`: old and new

| Old | New (L-6) |
|---|---|
| `struct NodeView { NodeSpec spec; Ports ports; NodeState state; std::optional<FailureReason> reason; };` | `struct NodeView { NodeSpec spec; Ports ports; NodeHealth health; std::optional<std::chrono::system_clock::time_point> since; };` (`spec.disabled` is the wire's `disabled`; `since` is filled by `MainLoopGateway`, 5.4; `GraphStatus::read` leaves it empty and takes `health` from `NodeHost::health()` by name, or, for a wanted node `NodeHost` has not told yet, `{phase added or disabled, state starting or disabled, wanted running or disabled}` from the spec) |
| `struct NodeAdded { NodeSpec node; Ports ports; };` | `struct NodeAdded { NodeSpec node; Ports ports; NodeHealth health; std::chrono::system_clock::time_point since; };` (health from the spec alone, D-M12) |
| `struct NodeUpdated { NodeName node; NodeSettings settings; };` | `struct NodeUpdated { NodeName node; NodeSettings settings; bool disabled; NodeOutcome applied; };` |
| `struct NodeChanged { NodeHealth health; };` | `struct NodeChanged { NodeHealth health; std::chrono::system_clock::time_point since; };` |
| `GraphEvent = std::variant<NodeAdded, NodeUpdated, NodeRemoved, NodeChanged, NodeStopped, RouteAdded, RouteRemoved, RouteChanged>` | adds `struct NodeGone { NodeName node; std::uint64_t droppedFrames; };` after `NodeStopped` |
| `using WallClock = ...;` in `status_log.hpp` | moved to `requests.hpp` (the edge both `StatusLog` and the gateway include) |
| `status_log.cpp`'s private UTC formatter | `std::string utcMicros(std::chrono::system_clock::time_point at);` in `graph_json.hpp`, the one home of the wire's and the records' instants (D-M21) |

`encodeEvent` per event: `NodeAdded` → `node.added` with 5.3's node members; `NodeUpdated` → `{"name", "settings",
"applied": outcomeName(applied), "disabled": true}` (`disabled` only when true; `outcomeName` gives `restart`,
`disable`, `enable`, `stored`, defined beside `encodeEvent`); `NodeChanged` → `node.state` with 5.3's health members;
`NodeStopped` → `node.stopped` (unchanged, deprecated); `NodeGone` → `node.gone` with the same params.

### 5.3 Health members, encoded once

One private `void addHealth(json& into, const NodeHealth& h, std::optional<time_point> since)` in `events.cpp` serves
snapshots, `node.added` and `node.state`, so the three cannot drift:

| Member | From | Present |
|---|---|---|
| `state` | `stateName(h.state)` | always |
| `phase` | `phaseName(h.phase)` | always |
| `since` | `utcMicros(*since)` | when known (always on `node.state` and `node.added`; on a snapshot once the gateway told the node) |
| `wanted` | `wantedName(h.wanted)` | always |
| `blockLost` | `h.blockLost` | always (D-M10) |
| `cause` | `{"kind": causeName(kind)}`, plus `"keys"` when `kind` is `settings`; with `restart`: `"restart": {"kind": causeName(*restart), "keys"?}` | when `h.cause` |
| `waitingFor` | `[{"kind": waitingKindName(kind), "name"?}]`, `name` for `older-node` and `companion` | when not empty |
| `reason` | `{"code": failureCode(code), "detail"}` (`lost` among the codes) | when `h.reason` |
| `disabled` | `true` | snapshots and `node.added` only, when `spec.disabled` |

### 5.4 `MainLoopGateway`: old and new

```cpp
// New (L-6): the clock that stamps since, and one more observer override.
MainLoopGateway(Executor& mainLoop, Executor& control, GraphRequests& requests,
                WallClock clock = &std::chrono::system_clock::now);   // all outlive it
/// DesiredNodeListener (observer): a node kept by name whose spec changed: posts NodeUpdated with the outcome.
void nodeChanged(const NodeSpec& node, std::shared_ptr<const NodeConfig> config, const NodeChange& change) override;
/// NodeHealthListener: the wire keys health by name and holds the newer node (L-3d, D-L36).
bool hearsSuperseded() const noexcept override { return false; }
```

- `since` is the gateway's (D-L22, D-M11): a main-loop `std::map<NodeName, {NodePhase phase; time_point since}>`;
  `nodeAdded` stamps `clock()` with the spec's phase; `nodeChanged(health)` stamps `clock()` only when the phase differs
  from the one held, else keeps it; `nodeGone` and a `nodeRemoved` with no later `nodeAdded` of the name erase it.
- `run`'s reply lambda, which runs on the main loop before it posts `done`, fills every `GraphView`'s `NodeView::since`
  from that map, so a snapshot's `since` is the one its `node.state`s carried.
- `nodeGone(name, dropped)` posts `NodeStopped` then `NodeGone`, in that order (control-plane.md 4.8).

### 5.5 `StatusLog`: the `action` record

`StatusLog` gains `public ActionListener` and `void actionAnswered(const ActionRecord& r) override`, which emits
`kind` `action` with fields `node`, `action`, `result`, `detail` (when set) and `msg`: `node <n> <action> invoked` when
`result` is `ok`, else `node <n> <action> refused: <result>` followed by ` (<detail>)` when there is one (D-M16):

```json
{"v":1,"seq":127,"t":"2026-11-09T10:05:12.770000Z","kind":"action","node":"desk","action":"restart","result":"ok","msg":"node desk restart invoked"}
{"v":1,"seq":128,"t":"2026-11-09T10:05:13.000000Z","kind":"action","node":"desk","action":"restart","result":"not-restartable","detail":"block-lost","msg":"node desk restart refused: not-restartable (block-lost)"}
{"v":1,"seq":129,"t":"2026-11-09T10:05:14.000000Z","kind":"action","node":"porch","action":"start","result":"node-disabled","msg":"node porch start refused: node-disabled"}
```

`action` records are not paced (only `log` records are). AGENTS.md's record table gains the line.

### 5.6 `NodeOperations`: old and new (D-M8)

control-plane.md 9.1 orders the `action` record before the `node` records the request causes, but `NodeOperations`
returns its reply only after every health change it caused was told. So L-6 gives each call a callback the aggregate's
judgement reaches before any action is carried out:

```cpp
// Old (L-3d)
virtual LifecycleReply start(const NodeName& node) = 0;   // and stop, restart
// New (L-6)
/// Called once, inside the call, with the reply the aggregate judged, before any hook, barrier or health change it
/// causes; null: nobody is told. A refusal calls it too (no health change follows).
using OperationJudged = std::move_only_function<void(const LifecycleReply& reply)>;
virtual LifecycleReply start(const NodeName& node, OperationJudged judged = {}) = 0;   // and stop, restart
```

`NodeHost` calls `judged` right after `NodeAggregate::stop()` (or `start`, `restart`) returned its `Operated` and before
it carries out `Operated::actions`; for a refusal it makes itself (`unknown-node`, `stopping` during `stopAll`) it calls
it before returning. L-3d's tests, which pass no callback, keep compiling.

### 5.7 `ear_config`: the keeper's step and `MainLoopSide`

**The keeper** (`keeper.hpp`, `keeper.cpp`). `take` gains the two edits; `advance` gains the lifecycle verbs as items
that ride with the reads (D-M13):

```cpp
// keeper.hpp, New (L-6)
struct Read {                                                            // was {index; optional<FileState> file}
	std::size_t index;
	std::variant<std::monostate /* GetGraph */, FileState /* GetGraphFile */,
	             std::pair<control::LifecycleVerb, NodeName> /* StartNode, StopNode, RestartNode */> what;
};
void postStep(Batch batch, std::vector<Read> reads, std::optional<Offer> offer, std::size_t offerIndex,
              std::optional<NodeName> appliedFor, bool last);         // appliedFor: the node of a DisableNode or
                                                                       // EnableNode whose candidate this offer is
```

- `advance`: `StartNode`, `StopNode` and `RestartNode` become `Read`s in index order (so a verb after an edit in the
  same batch waits for that edit's step, and one before it goes first), whatever the working copy's errors, and while
  stopped (`stopped_`) too.
- `take`: `DisableNode` and `EnableNode` are edits (`FileInvalid` while the working copy has errors; `Stopping` once
  stopped): `working_.disableNode(node)` or `enableNode(node)`; a refusal as any edit's; no candidate →
  `Applied{unchanged}`; a candidate → an offer with `appliedFor = node`.
- `postStep` on the main loop: each `Read` in order (`side.graph()`, `side.graphFile(file)`, or `side.operate(verb,
  node, stopped)`), then the offer; an accepted offer with `appliedFor` answers `Applied{kind}` from
  `judged.outcomes` (`disable` → `disable`, `enable` → `enable`, `stored` → `stored`; the node absent → `unchanged`),
  else the offer's result as today.

**`MainLoopSide`** (`main_loop_side.hpp`):

```cpp
// New (L-6)
struct Judged {
	Verdict verdict;
	std::expected<void, CommandError> result;
	std::vector<std::pair<NodeName, NodeOutcome>> outcomes;   // Plan::outcomes() of an accepted offer
};
MainLoopSide(DesiredGraph& graph, const GraphReader& reader, NodeOperations& operations);   // all outlive it
void subscribeActions(control::ActionListener& listener);   // main thread, before the first offer
/// A lifecycle verb on the main loop: `stopping` (the keeper stopped) answers CommandError Stopping without asking;
/// else NodeOperations' call with a judged callback that tells every ActionListener the record before any health
/// change. Returns the reply mapped by 5.8.
control::CommandResult operate(control::LifecycleVerb verb, const NodeName& node, bool stopping);
```

`GraphFileKeeper`'s constructor gains `NodeOperations& operations` (after `reader`) and it gains
`void subscribeActions(control::ActionListener& listener)` (main thread, before `start()`), both passed to its
`MainLoopSide`. `main.cpp` passes `nodeHost` and subscribes `statusLog`.

### 5.8 The mapping: core's reply to the wire

`MainLoopSide::operate`'s private `CommandResult resultOf(const NodeName&, const LifecycleReply&)` and
`ActionRecord recordOf(LifecycleVerb, const NodeName&, const LifecycleReply&)`, the one home of this table (tested in
`ear_config_tests` through a fake `NodeOperations`, node-lifecycle.md 11.4):

| `LifecycleReply` | `CommandResult` | Reply on the wire | `action` record `result` (`detail`) |
|---|---|---|---|
| `start` | `Applied{start}` | `{"applied":"start"}` | `ok` |
| `stop` | `Applied{stop}` | `{"applied":"stop"}` | `ok` |
| `restart` | `Applied{restart}` | `{"applied":"restart"}` | `ok` |
| `retry` | `Applied{retry}` | `{"applied":"retry"}` | `ok` |
| `unchanged` | `Applied{unchanged}` | `{"applied":"unchanged"}` | `ok` |
| `unknownNode` | `std::unexpected(CommandError{UnknownNode{node}})` | `-32000`, `unknown-node <n>`, `data.code` `unknown-node` | `unknown-node` |
| `nodeDisabled` | `ControlRefusal{NodeDisabled{node}}` | `-32000`, `node-disabled <n>`, `data.code` `node-disabled` | `node-disabled` |
| `notRestartable`, `blockLost` | `ControlRefusal{NotRestartable{node, blockLost}}` | `-32000`, `not-restartable <n>: block-lost`, `data` `{code, reason: "block-lost"}` | `not-restartable` (`block-lost`) |
| `notRestartable`, `notStarted` | `ControlRefusal{NotRestartable{node, notStarted}}` | as above with `not-started` | `not-restartable` (`not-started`) |
| `stopping` (and the keeper stopped) | `std::unexpected(CommandError{Stopping{}})` | `-32000`, `stopping`, `data.code` `stopping` | `stopping` |

`node.disable` and `node.enable` write no `action` record (they are edits: their `graph-file` and `node` records say
them, control-plane.md 9).

### 5.9 The wire, request by request

Every shape is control-plane.md's; this lists each with the C++ that makes it. `→` is the reply; `⇒` notifications in
order, each after the one before, all before the reply when the reply comes in the same turn (4.3's rule 5).

```json
{"jsonrpc":"2.0","id":1,"method":"node.stop","params":{"name":"porch"}}
⇒ {"jsonrpc":"2.0","method":"node.state","params":{"name":"porch","state":"stopping","phase":"stopping","since":"2026-11-09T10:09:29.990100Z","wanted":"stopped","blockLost":false,"cause":{"kind":"stop"}}}
→ {"jsonrpc":"2.0","id":1,"result":{"applied":"stop"}}
⇒ {"jsonrpc":"2.0","method":"node.state","params":{"name":"porch","state":"stopped","phase":"stopped","since":"2026-11-09T10:09:30.000000Z","wanted":"stopped","blockLost":false}}

{"jsonrpc":"2.0","id":2,"method":"node.start","params":{"name":"porch"}}
⇒ node.state starting (phase starting, wanted running)   → {"applied":"start"}   ⇒ node.state running

{"jsonrpc":"2.0","id":3,"method":"node.restart","params":{"name":"desk"}}
⇒ node.state stopping, "cause":{"kind":"restarting","restart":{"kind":"action"}}   → {"applied":"restart"}
⇒ node.state configuring, "cause":{"kind":"action"}   ⇒ configured   ⇒ starting   ⇒ running

{"jsonrpc":"2.0","id":4,"method":"node.disable","params":{"name":"porch"}}
⇒ graphFile.changed (cause edit)   ⇒ {"method":"node.updated","params":{"name":"porch","settings":{...},"applied":"disable","disabled":true}}
⇒ node.state stopping, "cause":{"kind":"disabling"}   → {"applied":"disable"}   ⇒ node.state disabled ("wanted":"disabled")

{"jsonrpc":"2.0","id":5,"method":"node.enable","params":{"name":"porch"}}
⇒ graphFile.changed   ⇒ node.updated {"applied":"enable"} (no "disabled")   ⇒ node.state configuring, "cause":{"kind":"enabling"}
→ {"applied":"enable"}   ⇒ configured   ⇒ starting   ⇒ running

{"jsonrpc":"2.0","id":6,"method":"node.add","params":{"name":"noise","type":"white","disabled":true}}
⇒ graphFile.changed   ⇒ node.added {..., "state":"disabled","phase":"disabled","wanted":"disabled","blockLost":false,"disabled":true,"since":"..."}   → {}

{"jsonrpc":"2.0","id":7,"method":"node.remove","params":{"name":"rec"}}
⇒ graphFile.changed   ⇒ route.removed ...   ⇒ node.removed   → {}   ⇒ (ignored by the client) node.state stopping, node.stopped, node.gone

{"jsonrpc":"2.0","id":8,"method":"graph.get"}
→ {"protocol":1,"features":["lifecycle"],"graphFrequency":48000,"blockSize":256,"realtime":true,"scheduling":"SCHED_FIFO 88","types":[...],
   "nodes":[{"name":"porch","type":"card","settings":{...},"inputs":[...],"outputs":[...],"state":"stopped","phase":"stopped","since":"...","wanted":"stopped","blockLost":false}],"routes":[...]}

Refusals:
→ {"jsonrpc":"2.0","id":9,"error":{"code":-32000,"message":"node-disabled porch","data":{"code":"node-disabled"}}}
→ {"jsonrpc":"2.0","id":10,"error":{"code":-32000,"message":"not-restartable desk: block-lost","data":{"code":"not-restartable","reason":"block-lost"}}}
→ {"jsonrpc":"2.0","id":11,"error":{"code":-32000,"message":"unknown-node ghost","data":{"code":"unknown-node"}}}
→ {"jsonrpc":"2.0","id":12,"error":{"code":-32602,"message":"/colour: unknown","data":{"pointer":"/colour"}}}
```

A `node.stop` on a `failed` node, or on one still `added` or `configuring`, sends one `node.state` with the new `wanted`
and the same phase, then the reply. A `node.restart` of a `failed` node replies `retry` after `configuring`.

### 5.10 Fixtures

As node-lifecycle.md 9 lists them, made in this step with the encoders: `lifecycle.json` (control-plane.md 4.7's list,
3a's phases), `unknown-values.json`, `errors.json` (`node-disabled`, both `not-restartable` reasons, the unknown request
member), `snapshot.json` and `edits.json` regenerated (`protocol`, `features`, the health members, `disabled`,
`node.updated`'s `applied`), `output-records.json`'s `action` cases (the three of 5.5). `ear_control_tests` read them
field for field; the editor's reducer tests read the same files.

### 5.11 The editor's minimum

control-plane.md 8.6 is the home of what each phase shows; these are the TypeScript signatures L-6 adds (`protocol.ts`'s
header names batch2.md's erratum and control-plane.md as the wire's homes from protocol 1):

```ts
// protocol.ts: every enum open (a known union plus (string & {})), with a neutral fallback where drawn.
export type NodeState = "starting" | "running" | "stopping" | "stopped" | "disabled" | "failed" | (string & {});
export type NodePhase = "added" | "configuring" | "configured" | "starting" | "running" | "stopping" | "stopped"
  | "disabled" | "failed" | (string & {});
export type Wanted = "running" | "stopped" | "disabled" | (string & {});
export type CauseKind = "stop" | "removing" | "disabling" | "restarting" | "settings" | "action" | "retry"
  | "enabling" | (string & {});
export type FailureCode = "engine-refused" | "start-failed" | "lost" | (string & {});
export type Applied = "start" | "stop" | "restart" | "retry" | "disable" | "enable" | "stored" | "unchanged"
  | (string & {});
export interface NodeCause { kind: CauseKind; keys?: string[]; restart?: { kind: CauseKind; keys?: string[] } }
export interface WaitingFor { kind: "older-node" | "block" | "companion" | "consumer" | (string & {}); name?: string }
export interface NodeHealth {
  state: NodeState; phase: NodePhase; since?: string; wanted: Wanted; blockLost: boolean;
  cause?: NodeCause; waitingFor?: WaitingFor[]; reason?: FailureReason;
}
export interface GraphNode extends NodeHealth { name: string; type: string; settings: JsonObject; inputs: string[];
  outputs: string[]; disabled?: true }
export interface Graph { protocol?: number; features?: string[]; /* today's members */ }
// Methods gains:
//   "node.start" | "node.stop" | "node.restart" | "node.disable" | "node.enable":
//     { params: { name: string }; result: { applied: Applied } }
//   "node.add": params NodeSpec & { disabled?: boolean }
// Notifications: "node.added": GraphNode; "node.updated": { name; settings; applied?: Applied; disabled?: true };
//   "node.state": { name: string } & NodeHealth; "node.gone": { name: string; droppedFrames: number }
// ControlErrorCode = "node-disabled" | "not-restartable" | (string & {}) beside CommandErrorCode and FileErrorCode.

// health.ts (new): the one place that turns health into words, style and enabled buttons.
export interface ActionAvailability { enabled: boolean; why?: string }
export interface HealthView {
  word: string;                 // the state word, or the phase's word for an unknown state
  styleClass: string;           // "state-<state>" for known states, "state-unknown" otherwise
  line: string;                 // 8.6's line ("stopped until Start or a restart of ear", "waiting for fpp.rtp")
  actions: { start: ActionAvailability; stop: ActionAvailability; restart: ActionAvailability;
             disable: ActionAvailability; enable: ActionAvailability };
}
/** `now` (ms since the epoch) hides a starting phase's line for its first 250 ms. */
export function describeHealth(node: GraphNode, now: number): HealthView;
```

`graph.ts`'s reducer folds `node.state` by replacing every health member (an absent one is absent now), ignores
`node.gone` and `node.stopped` for an unknown node as it does today, and takes `node.updated`'s `disabled`;
`EarNode.tsx` draws `describeHealth`; `editor.css` styles `state-stopped`, `state-disabled` and `state-unknown`;
`Panels.tsx` gains Stop, Start, Restart, Disable and Enable, each enabled by `describeHealth` and gated on
`features.includes("lifecycle")`. Vitest: `lifecycle.json` and `unknown-values.json` through the reducer; `health.ts`
for every phase (its line, style and enabled actions, the 250 ms hide, an unknown phase drawn by its word).

`scripts/rpc-call.mjs` gains `--until <state>` and `--timeout <ms>` (exit 0 when the node reaches the state, 1 on
`failed`, 2 on timeout, control-plane.md 2.2); `scripts/image-smoke.sh` checks `protocol` and `features`.

### 5.12 Tests

node-lifecycle.md 11.4 and 11.5 name them: `ear_config_tests` (lifecycle verbs ordered with edits, writing nothing, while
the file has errors and while stopping; disable and enable through the keeper with `applied`; 5.8's table through a
fake `NodeOperations`, the `action` record told before the fake's health); `ear_control_tests` (the five commands
decoded, `disabled` on `node.add`, `data.pointer`, results and refusals encoded, `node.gone` after `node.stopped`,
`since` kept while only `wanted` changes, the fixtures); `ear_system_tests`' control-port cases wait for `node.gone`.

## 6. L-8: what closes slice 3a

- **System tests** (`test/system/`, event-gated): node-lifecycle.md 12's L-8 list and control-plane.md L1's: `node.stop`
  writes nothing and a restart of ear runs the node again; `node.disable` turns `node white noise;` into `node white
  noise { disabled true; }`, survives a restart of ear (`node noise disabled` at boot, no `starting` before it), and
  `node.enable` gives `node white noise;` back; `node.disable` of one of two nodes sharing `disabled $quiet;` replaces
  only its line, the other playing on; `node.restart` of a `card` node on the test card (records `action`, `stopping:
  restarting (restart invoked)`, `running`); an `rtp` node on a held UDP port writes `node fpp still configuring after
  2s: waiting for fpp.rtp` and `graph.get` shows `waitingFor`; `node white noise disabled;` by hand boots nothing and
  shows `ear/bad-directive` with the hint.
- **AGENTS.md**: the record table (`stopping: <cause>`, `stopped`, `disabled`, `removed`, `still`, `action`,
  `ear.fade-unacknowledged`, `ear.barrier-overdue`, `ear.data-loop-wedged`, `ear.hook-slow`, the stall records); the
  control port's methods (the five verbs, `node.gone`, `protocol`, `features`); the Architecture table's `ear_core` row
  (`lifecycle.hpp`, `sink.hpp`, the machines, `NodeOperations`; `run_requests.hpp` gone) and `ear_pipewire`'s
  (`ThreadSinks`); "Adding a node type" step 3 rewritten for the class (`ports`, optional `check`, the constructor,
  `configure(CreateOf<spec>&, const Checked&)` acquiring with `cx.soundCard`, `cx.rtpReceiver`, `cx.openRecording`,
  `cx.sink(...).consumedBy(...)` and `cx.declareLatency`, then `cx.use`, answering `Configured`; optional `start()` and
  `stop()`; the change version slice 3d's) and step 4 for the new harness's calls; the Tests table's rows for
  `ear_core_tests`, `ear_node_*_tests`, `ear_pipewire_tests` (`sink_test.cpp`) and `ear_control_tests`; the
  `file` and `sendspin` paragraphs (a stop keeps sendspin's connection; `file` opens its WAV on the sink's thread).
- **Skills**: `pipewire-engine` (asynchronous drive, fades, the taps, the wedge, `context.num-data-loops`), `control-port`
  (the five verbs, `node.gone`, `--until`, the keeper's no-file step, `action` records).
- **node-framework.md**: "Slice 3a as built" (what each step built, the errata this page's decisions make: D-M2's 3a
  refusal of the change version, D-M3, D-M5, D-M6, D-M8, D-M10).
- **node-lifecycle.md**: its review record's L-4a, L-4b and L-6 entries; its section 6 rows for `sink.hpp` and
  `lifecycle_wire.hpp` (D-M4).
- **batch2.md section 11**: the erratum naming control-plane.md the wire's home from protocol 1 (L-6 adds it; L-8
  checks every frozen header's first line names its page).

## 7. Every change to an existing file, by step

| File | Step | Change |
|---|---|---|
| `src/core/include/ear/core/node_type.hpp` | L-4a | `RunCode` → `NodeCode` (alias kept); `NodeConfig::start` default; `NodeConfig::make` (3.5) |
| `src/core/include/ear/core/sound_card.hpp`, `rtp_receiver.hpp` | L-4a | `bank{}` (D-M15) |
| `src/core/include/ear/core/type_spec.hpp` | L-4a | `SpecConfig`, `SpecNodeType`, `makeNodeType` moved to `lifecycle.hpp` (3.6); header comment's example |
| `src/core/include/ear/core/lifecycle.hpp` | L-4a | the alias flips to `node_type.hpp`; gains the moved three |
| `src/core/include/ear/core/node_host.hpp` | L-4a | `hookSlowBound`; `nodeAdded`'s comment names `configure` and the instance |
| `src/core/node_host.cpp` | L-4a, L-4b | 3.3, 3.4, 4.2 |
| `src/core/legacy_instance.hpp`, `.cpp` (new, private) | L-4a | `LegacyInstance` (3.5) |
| `src/core/include/ear/core/run_requests.hpp`, `run_requests.cpp` | L-4a | deleted (3.7) |
| `src/core/include/ear/testing/node_harness.hpp`, `type_harness.hpp`, `testing/node_harness.cpp` | L-4a, L-4b | replaced by 3.8's classes; `lifecycle_harness.hpp` deleted |
| `src/core/tests/compile_fail/type_spec_bad.cpp`, `src/core/CMakeLists.txt` | L-4a | 3.10's cases |
| `src/nodes/white`, `card`, `rtp` | L-4a | 3.9 |
| `src/core/include/ear/core/node_host.hpp` | L-4b | `DriveRequest::sinks`, `ResourceRegistry`'s sink adapter, `nodeGone`'s comment (4.2) |
| `src/pipewire/include/ear/pipewire/handoffs.hpp`, `handoffs.cpp` | L-4b | `ThreadSinks` (4.3) |
| `src/pipewire/port_blocks.cpp`, `port_blocks.hpp` | L-4b | the taps (4.4) |
| `src/nodes/file` | L-4b | 4.5 |
| `src/main.cpp` | L-4b, L-6 | `ThreadSinks`; `NodeOperations` and `subscribeActions` to the keeper |
| `src/core/include/ear/core/node_host.hpp` | L-6 | `OperationJudged` on `NodeOperations` (5.6) |
| `src/core/include/ear/core/graph_view.hpp`, `graph_status.cpp` | L-6 | `NodeView` (5.2) |
| `src/control/include/ear/control/commands.hpp`, `commands.cpp`, `graph_json.hpp`, `graph_json.cpp` | L-6 | 5.1, `utcMicros` |
| `src/control/include/ear/control/events.hpp`, `events.cpp` | L-6 | 5.2, 5.3 |
| `src/control/include/ear/control/gateway.hpp`, `gateway.cpp` | L-6 | 5.4 |
| `src/control/include/ear/control/requests.hpp`, `status_log.hpp`, `status_log.cpp` | L-6 | `WallClock` moved; `ActionListener` (5.5) |
| `src/control/session.cpp` | L-6 | nothing but the new commands passing through (graph.subscribe's snapshot carries `since`) |
| `src/config/keeper.hpp`, `keeper.cpp`, `main_loop_side.hpp`, `main_loop_side.cpp`, `include/ear/config/graph_file_keeper.hpp`, `graph_file_keeper.cpp` | L-6 | 5.7, 5.8 |
| `test/protocol/*.json` | L-4a, L-6 | `ear.hook-slow` (L-4a); 5.10 (L-6) |
| `web/src/` | L-6 | 5.11 |
| `scripts/rpc-call.mjs`, `scripts/image-smoke.sh` | L-6 | 5.11 |

Deleted over the three steps: `run_requests.hpp` and `.cpp`; `lifecycle_harness.hpp`; `Recorder`, `FileRun`; every type's
static `start`; `SpecConfig::start`; the slice 2 harness classes. Interim, and going later (node-lifecycle.md 13):
`LegacyInstance` and the old SPI (3c), `DriveRequest::sinks` and `SinkTap` (3b), `CreateOf`'s interim calls (3b, 3c).

## 8. Decided by the architect

Each is technical or internal; the owner can overturn any. Section 10 asks what is theirs.

| # | Decision | Why |
|---|---|---|
| D-M1 | `lifecycle.hpp` is the type author's API and, from L-4a, the home of `SpecConfig`, `SpecNodeType` and `makeNodeType`; `type_spec.hpp` keeps the spec vocabulary | They now make an instance, which needs the contexts; `type_spec.hpp` including `lifecycle.hpp` would be a cycle, since `CreateOf` needs bank handles and `CheckedOf` |
| D-M2 | The owner's answer to Q1 (2026-11-09: "Accept, ignore until 3d"): a class that writes the change version compiles, and 3a never calls it: every change restarts the node. `SpecInstance` checks only its signature (a change version of the wrong shape still does not compile), `ChangeOf<Spec>` is declared so the signature can name it, and AGENTS.md's "Adding a node type" says the change version is ignored until slice 3d. L-4a builds this; no type is given one before 3d | The owner chose it over this page's recommendation (refuse until 3d) |
| D-M3 | Every rule a type breaks inside or at the return of `configure` fails the node `start-failed` with the rule's text; none escapes `NodeHost`, D-L40's check included since L-4a's review (it supersedes D-L40's throw) | node-framework.md 6.10 says each is a `std::logic_error`; 6.4 says a hook that throws is caught and "one type's bug never takes ear down". A `logic_error` thrown at the call escapes `configure` and is caught as its throw; the return checks make the same failure, so both readings hold |
| D-M4 | The sink's declarations have one header, `sink.hpp`, not `node_type.hpp` and `node_host.hpp` as node-lifecycle.md 6's table placed them | One home for the sink; `lifecycle.hpp` needs `SinkConsumer` without `node_host.hpp`; it lands without touching two frozen headers L-3d is editing |
| D-M5 | `SinkConsumer` has a public virtual destructor | node-primitives.md 6.6 gives it a protected non-virtual one, but `consumedBy(std::unique_ptr<SinkConsumer>)` deletes it through the base, which does not compile with that destructor |
| D-M6 | `ErasedInstance::ready` returns `std::expected<void, FailureReason>`; `hasRecipe()` is added | 5.10 makes a failed legacy recipe a fault, which a `void` cannot report; D-L40's check needs to know a recipe exists |
| D-M7 | `ear.hook-slow` names the hook in its detail (`configure ran 31 ms`) with the field `ms`; reported once per node and hook for the `Node`'s life; the instance's making counts as `configure`, a legacy `start` too; the recipe is not a hook | `NodeDiagnosticListener` has no subject until slice 5's diagnostics model; a restart that is slow again says nothing new |
| D-M8 | `NodeOperations`' calls gain a defaulted `OperationJudged` callback, called with the judged reply before any action | control-plane.md 9.1 orders the `action` record before the `node` records it causes, but 5.2's reply comes after every tell; the aggregate judges before it acts, so the reply is known in time |
| D-M9 | `node.update`'s reply gains `applied` with step L3 (the `node.set` feature), not L-6; `AppliedKind` already holds `stored` | control-plane.md 4.2 gates it on `node.set`; 4.7's `edits.json` row lists it in the page-wide regeneration. L-6 adds `applied` to the five lifecycle replies and `node.updated` only |
| D-M10 | `node.state` carries `blockLost` always | control-plane.md 4.8's member list omits it, but 4.4 says `node.state` replaces every health member and 8.6 disables Start once the block is lost; without it a client learns the loss only from a new snapshot |
| D-M11 | `since` is stamped by `MainLoopGateway` when a node's phase changes (not when only `wanted` or `waitingFor` does) and filled into snapshots in the reply on the main loop | D-L22 makes `since` the listener's; the snapshot comes through the keeper's reply, which the gateway posts, so one map gives both the same instant |
| D-M12 | `node.added`'s health comes from the spec alone (`added` or `disabled`; `wanted` `running` or `disabled`), and `NodeHost`'s first `node.state` follows in the same turn | The gateway hears `nodeAdded` before `NodeHost` judges the node; a port-caused successor carrying a stop shows `wanted: running` for one message, corrected by the next |
| D-M13 | The lifecycle verbs ride with reads in the keeper's step, in index order; once the keeper stopped they still reach the main loop, where `MainLoopSide` answers `stopping` without asking `NodeHost` and writes the record | Ordering with edits (D-L19) for free; between the graph's close and `stopAll` `NodeHost` would otherwise apply a start |
| D-M14 | The harness does not observe `start()` and `stop()`; their pairing is tested in `node_host_test.cpp` with a probe class | A real type's hooks are invisible to a harness that runs the real `NodeHost` (node-lifecycle.md 11.3 asked `harness_test` to check it) |
| D-M15 | `SoundCardRequest::bank` and `RtpReceiverRequest::bank` default to empty | The type leaves them out (the handle names the bank); without the default GCC warns at every designated initializer under `-W` |
| D-M16 | A refused lifecycle request's `action` record: `result` the code, `detail` the reason (`not-restartable` only), `msg` `node <n> <verb> refused: <code>[ (<reason>)]` | control-plane.md 9 shows only a type action's refusal; this follows its shape |
| D-M17 | A sink whose adapter throws (no thread) fails the node through `configure`'s catch | No new failure path; the operator sees `configure threw: <what>` |
| D-M18 | The new harness lives in `ear::testing::next` (`lifecycle_harness.hpp`) until L-4a merges it into `node_harness.hpp` and `type_harness.hpp` | Today's tests hold the names; the header compiles and is checked now |
| D-M19 | A code written against another type's spec stays a run-time refusal (now at `use`), not a compile-time one | Slice 2's rule and its test keep their meaning; a handle of the same name, direction and reach is the same handle by design |
| D-M20 | `unknown-node` and `stopping` replies to the verbs are `CommandError`s (today's codes); `node-disabled` and `not-restartable` are `ControlRefusal` | One spelling per code; control-plane.md 4.5 puts the new codes in `ControlRefusal` |
| D-M21 | `WallClock` moves to `requests.hpp`; `utcMicros` in `graph_json.hpp` is the one home of an instant's text | The gateway now stamps `since` in the records' format |
| D-M22 | `protocol` and `features` are constants in `commands.hpp`, not members of `GraphView` | They are the encoder's facts, not the graph's |

## 9. Where the higher pages are ambiguous or disagree

1. **`SinkConsumer`'s destructor** (node-primitives.md 6.6 against node-framework.md 6.10's `consumedBy(std::unique_ptr<SinkConsumer>)`): incompatible as written. D-M5; node-primitives.md 6.6 needs an erratum.
2. **A broken rule: thrown out, or a failure?** node-framework.md 6.10 ("each a `std::logic_error`") against 6.4 ("the framework catches it, fails the node"), and node-lifecycle.md 5.9's table ("is a `std::logic_error`") against its own edges. D-M3.
3. **`node.state`'s members** omit `blockLost` (control-plane.md 4.8) while 4.4 replaces every health member and 8.6 needs it. D-M10.
4. **The `action` record's order** (control-plane.md 9.1: before the `stopping` it causes) against `NodeOperations`' reply after every tell (node-lifecycle.md 5.2). D-M8.
5. **`node.update`'s `applied`**: control-plane.md 4.2 gates it on `node.set` (L3); 4.7 lists it among L1's regenerated fixtures. D-M9.
6. **Where the sink is declared**: node-lifecycle.md 6 puts `Chunk` and `SinkConsumer` in `node_type.hpp` and the adapter in `node_host.hpp`; this page gives them one header. D-M4.
7. **`ErasedInstance::ready`** returns `void` in node-lifecycle.md 5.9 while 5.10 makes a failed recipe a fault. D-M6.
8. **`stop()`'s pairing in `harness_test`** (node-lifecycle.md 11.3) is not observable through the harness. D-M14.
9. **`ear.hook-slow`'s subject** (control-plane.md 4.6: the hook) has no place in today's `NodeDiagnosticListener`. D-M7.
10. **Who stamps `since` for a snapshot** (D-L22 says each listener stamps it when told; a snapshot is not a tell). D-M11.
11. **`node.added` for a port-caused successor**: D-L36 says no `added` is told for it, but the wire's `node.added` (sent by the gateway as an observer at the commit) still says phase `added`. D-M12 accepts one message of it.
12. **The change version in 3a**: node-framework.md 6.4 calls the change version optional and lists its signature among the class's members, while 13.1 and node-lifecycle.md 2 put it in 3d; neither says what a 3a class that writes it gets. D-M2 and Q1.

Nothing found is wrong in the sense of unbuildable, but item 1, which does not compile as written.

## 10. Questions for the owner

**Q1 (answered 2026-11-09).** A type that writes the change version of `configure` before slice 3d: refuse it, or
accept and ignore it? The owner: "Accept, ignore until 3d". D-M2 records what L-4a builds. No question is open.

## 11. Review record

### Revision 1 (2026-11-09, not reviewed)

Written from the three higher pages and the code at `db6dd28`, with L-3d's in-flight `node_host.hpp` read for its
`NodeHealth` and `NodeOperations`. The four headers were compiled by the repository's header check, and a scratch
translation unit (not committed) compiled `white`, `card`, `rtp` and `file` as 3.9 and 4.5 write them against
`lifecycle.hpp`, instantiated each `SpecInstance` and `TypeHarness`'s typed calls, and saw 3.10's refusals fire for a
static `start`, the change version, a `configure` taking its settings by value, `int start()`, a constructor of neither
kind, a sink on an input bank, `hold`, `Configured::restart` and a backwards `declareLatency`.

### Found building L-4a (2026-11-09)

What step L-4a had to change or decide beyond the declarations above, each the smallest change that works, with its
reason. Nothing here changes a rule of a higher page.

1. **`NodeHost::subscribeLog(LogSink&)`** (`node_host.hpp`, new). node-lifecycle.md 5.9's edge (D-L45) makes a `stop()`
   that throws during a teardown a `log` record from source `ear`; `NodeHost` had no log seam, only its listeners.
   `main.cpp` subscribes `StatusLog`; a test subscribes a sink of its own.
2. **`makeLegacyNodeType<Spec, Behaviour>()`, `spec_detail::LegacySpecConfig` and `LegacySpecNodeType`**
   (`type_spec.hpp`, interim, with their friend declarations in `node_type.hpp` and `settings_table.hpp`). `file` stays on
   the legacy path until L-4b, but D-M1 moves `makeNodeType` to the class and `SpecInstance` refuses a static `start`;
   these are slice 2's `SpecConfig`, `SpecNodeType` and `makeNodeType` renamed, run through the legacy adapter. L-4b
   deletes them with `file`'s move.
3. **`NodeHarness::latencyPlan()`** (`node_harness.hpp`, new): the fake block's last `LatencyPlan`. 3.8's harness had no
   way to show what a configuration declared (slice 2's harness had `declaredLatency()`), so `rtp`'s latency test would
   have lost its check.
4. **`LegacyInstance::runObject()`** (private): the harness's `runObject()` reads it; the harness wraps each config it
   parses so it sees the instance each configuration makes and when it goes (it observes no hook, D-M14).
5. **`CreateContext::sink` before L-4b** refuses (`sink on bank "<bank>" is not built before step L-4b`, so a type that
   asks fails `start-failed: configure threw: ...`): no `SinkAdapter` is in `ResourceRegistry` until L-4b, and no type
   asks for a sink before it. `CompanionFake::Kind::sink` and `SeenChunk` stay declared, unused until then.
6. **3.10's change-version row** was stale after D-M2's amendment (it still refused the right shape). Built as
   `change-version-wrong-signature`, above; `type_spec_test.cpp` runs a class with the change version and sees a change
   restart it.
7. **3.4's example record** gave `msg` as `node rec ear.hook-slow ms=31: configure ran 31 ms`. `StatusLog`'s
   `diagnostic` record puts no field in its `msg` and carries `detail` (as `ear.fade-unacknowledged` does), so the
   fixture's case is `"ms":31,"detail":"configure ran 31 ms","msg":"node rec ear.hook-slow: configure ran 31 ms"`.
8. **The rules' texts.** 3.3 has a context throw `"<node>: <call> outside configure"`, 3.2's table gives the failure's
   detail as `<hook> threw: <call> outside configure`. Built as 3.2 says (no node name in the text: the record already
   names the node). A failure from a hook is `<hook> threw: <what>` with the hook `configure`, `start` or `stop`; the
   teardown's log text is 5.9's, `stop() threw: <what>`. A card's request naming another bank than its handle's:
   `a sound card's request names bank "<x>", not "<bank>"`.
9. **`NodeContext`'s name is a copy**, not a reference to the node's: a leaked configuration (I5a) may outlive its node,
   and its instance's `NodeOf` with it.
10. **Behaviour that changed, by D-M3.** A legacy type's bug at configure (a second engine object of a name, a bank of
    the wrong kind, latency twice for a bank, a pass-through the ports refuse) was a `std::logic_error` thrown out of
    `NodeHost`; it now fails the node `start-failed` (`configure threw: ...`, or `configure gave a pass-through: ...`),
    and `node_host_test.cpp`'s cases say so. (D-L40, a legacy recipe beside an outside node, still threw at this
    commit; the review below made it a failure too.)
11. **A configure that failed or threw keeps its recordings until step 5**, freed after its instance (5.9's edge), where
    the slice 2 path closed them at once; its handoffs still go at once (their consumers may have gone with a legacy
    start's result).
12. **A `start()` that threw** is a fault carried out once its batch was: the `Tell` of `running` that batch held is
    not told (4.2: it never reached running), so the node never shows `running` before `failed`.

### After review (L-4a, 2026-11-09)

An independent review of the L-4a commit asked for these; each is built, and the interface-level ones are named.

1. **Members found by name, whatever their access or overloads** (`lifecycle.hpp`). `requires { &Type::start; }` is false
   for a private, overloaded or template member, so such a `start`, `stop` or `check` was silently never called (a
   private `check` let invalid settings through, inherited from slice 2). `spec_detail::NameProbe` (a probe deriving from
   the class and from a struct declaring the name: the lookup is ambiguous only when the class declares it too) finds the
   name, then `static_assert`s it is public and single ("a type's start is public and not overloaded", likewise stop
   and check) and of its exact shape. **New rule:** a type's class is a class and not final ("a type's class is a class,
   not final"), which the probe needs. The change version is now found by any three-parameter `configure`
   (`spec_detail::AnyContext`), so one taking `CreateOf&` is refused too. Compile-fail cases: `private-start`,
   `private-stop`, `private-check`, `overloaded-start`, `change-version-wrong-context`, `class-final` (82 cases now).
2. **`Changed::applied()` and `restart(reason)` have trivial inline bodies** in 3a, so a change version defined in
   `<type>.cpp` links.
3. **A broken rule is recorded, not only thrown** (D-M3 made robust). **New: `CreateContext::refuse(std::string rule)`**,
   `[[noreturn]]`, which `CreateOf::use` calls for a code of another spec. Every context call runs guarded: the first
   rule it finds broken is recorded on the node for the hook running, and after `configure` (whatever it answered),
   `start()` or `stop()` returns the node fails with it (`configure threw: <rule>`), so a type that catches
   `std::exception` cannot swallow a rule and run half-acquired.
4. **D-L40 superseded by D-M3**: a legacy recipe beside an outside node fails the node `start-failed` with D-L40's text
   (node-lifecycle.md's D-L40 row says so); a legacy recipe that throws is the fault `recipe threw: <what>`
   (recipeFailed); a `RunObject::close` that throws is a `log` record (`close threw: <what>`, source `ear`) and the
   configuration is taken as closed. Nothing a type does throws out of `NodeHost` now.
5. **A legacy start's `engineRefused` keeps its fact.** **New: `ErasedInstance::engineRefused()`** (default false;
   `LegacyInstance` answers from its failed `StartResult`), so the node fails `engine-refused`, not `start-failed`.
6. **The harness shows legacy handoffs and recordings.** **New: `NodeHarness::handoffs()`** (`SeenHandoff`: bank,
   channels, capacity) **and `recordingRequests()`**, through a forwarding pair of contexts the harness hands each
   configure. `file_test.cpp`'s capacity (2 s) and path checks are back.
7. **`NodeConfig::start` is a throwing default, not pure** (node_type.hpp): an interim deviation from the frozen header's
   pure virtual, so `SpecConfig` need not define it; a legacy config that forgets to override it fails its node at
   configure (`configure threw: a type on the new SPI has no start`) instead of not compiling. It goes with the old SPI
   in 3c.
8. **A `start()` that threw** drops only the `Tell` of `running` from its batch, not every `Tell`.
9. **`cx.use` with no code** (a null code or a pass-through of no pair) is refused at the call (`use given no code: a null
   code or a pass-through of no pair`), and **`Configured::failed("")`** fails the node `start-failed: configure answered
   failed with no detail`.
10. **Tests added:** a rule caught by the type (in configure, and in `start()` through a kept context), `use` with no code
    and an empty failure, a legacy engine refusal, a throwing recipe and close, a `stop()` that throws during a disable
    and during a failure, a `start()` that throws on a start after a stop.

### Found building L-4b (2026-11-09)

What step L-4b had to change or decide beyond section 4's declarations, each the smallest change that works, with its
reason. Nothing here changes a rule of a higher page. The legacy maker (`makeLegacyNodeType`, `LegacySpecConfig`,
`LegacySpecNodeType` and their friend declarations) is deleted with `file`'s move, as Found building L-4a's item 2 said;
`sendspin` alone stays on `LegacyInstance` until 3c.

1. **A configure that failed frees its sinks unbegun, at once** (`failConfigure`), rather than closing them at the
   teardown. Sink's own contract allows a sink neither begun nor closed to be destroyed, and the configuration's
   `Channel` is detached there (a `closed` posted through it would never land). So "close works on a sink with no
   consumer" (5.11) holds trivially: nothing of such a sink is ever closed or finished; a consumer given is freed
   without `begin`. A failure after the return checks (a lost in `begin`, an outside node lost) closes every sink at
   I5 step 3 as 4.2 says, `finish` running only on a consumer that began.
2. **The rules' texts.** `consumedBy` with null: `consumedBy given no consumer on sink <bank>`; a sink on a bank that
   is no inside output (reachable only through the erased `CreateContext`, since `CreateOf` refuses it at compile time):
   `a sink joins an inside output bank, not "<bank>"`; a second sink on one bank is I9's
   `a second resource named <node>.<bank> in one configuration`, the same check an outside node's name goes through, so
   a sink on a bank named `playback` beside a playback card is refused too (the sink's own I9 check the task asked for).
3. **D-M17 made robust**: an adapter that throws (no thread) is recorded as the configuration's broken rule as well as
   thrown, so a type that catches it still fails `configure threw: <what>`, as D-M3 does for every other call.
4. **`FilterPortBlock::drive` throws `std::logic_error`** for a request that binds more sinks than the block has inside
   output banks: the taps are copied inside the invoke into storage sized at make (4.4: nothing allocates on the data
   loop), and a sink reads an inside output bank, so more is a caller's bug.
5. **`TypeHarness` forwards `health()`, `gone()`, `droppedFrames()`, `unwritten()`, `companions()`, `handoffs()` and
   `recordingRequests()`** to its `NodeHarness`, so a test that holds the harness by pointer (`file_test.cpp`'s
   `std::unique_ptr<TypeHarness>`) reads them without `->node()`. The harness's `FakeSinks` keeps `outsideEvents`
   parallel to `companions()` with a null entry per sink, so `ready(bank)` or `lose(bank)` on a sink's bank says it has
   no outside node to tell.
6. **A node whose only outputs are sink banks still fades before a silence** (`file`): `Queued::fades` is decided from
   the ports before configure knows its sinks, so the stop posts a fade with nothing to ramp, which a real block
   acknowledges at the next cycle's end. One cycle at a stop; the sink keeps taking the code's cycles until the silence,
   as it must.
7. **`waitingFor` `consumer`** is shown while configuring with a sink whose consumer has not begun (beside the block
   and companions), and `ear.close-overdue`'s detail counts open sinks when there are any (`... 1 sinks open, run ...`).
8. **`ResourceRegistry`'s three-argument form takes the sink adapter** (4.2), so every host passes one; core tests with
   no sink pass a refusing one (`graph_status_test.cpp`'s `NoSinks`), and `engine_test.cpp` passes a real `ThreadSinks`.
9. **The stamp ring's index**: a stamp is released after its chunk was consumed, so a consumer stalled inside one
   chunk holds that chunk's entry; the ring then takes `entries - 1` more before it drops whole chunks (`sink_test.cpp`
   counts exactly that).
10. **A sink's first chunk after a node's first drive is not at that drive's `takenAt`** when the block had run no cycle
    before it (`PortBlock::takenAt` is 0 until a cycle ran, and `NodeHost` drives at the block's first STREAMING): the
    chunk carries the clock's real position, so a consumer that dates by `position` is right; only a comparison with the
    drive's reported position is not. `sink_test.cpp`'s ramp therefore warms the block up with a first code, and judges
    a start's first frame exact unless the clock jumped (an xrun) at that take-up, as `port_block_test.cpp` does; a
    stop's last frame is judged exact always.
