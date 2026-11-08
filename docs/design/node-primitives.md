# Node primitives: slice 3c of the node framework

Status: proposal, revision 5, 2026-11-08. Revision 2 followed two independent reviews (section 20); revision 3 rebased
the page onto `docs/design/node-framework.md` revision 5 and was reconciled with that page's review round 3. Revision 4
rebases it onto the framework page's revision 6 ("the framework page" below), which took the owner's answers of
2026-11-08 (R19 to R24): there is no run; the graph always runs and takes asynchronous, ordered operations; a type
writes three hooks, `configure(old, new)`, `start()` and `stop()`; a node's own threads are its loops', reached from a
hook only by a typed post. Revision 4 also re-sorts this page's gates (section 18): every open question here was
technical, so each is now decided by the designer, with its reason, for the owner to overturn. Revision 5 follows the
framework page's revision 7, which took the owner's answers to its gates F1 and F2 (R25 to R28): a `stopped` node keeps
every primitive, as revision 4 said, and a new resting state, `disabled` (a word in `ear.conf`), releases every one by
the full teardown (6.1, 6.9, 6.10, 10). Revisions 4 and 5 are not reviewed yet. Nothing here is built.
Layer: module design, beneath the framework page. It designs that page's slice 3c (primitives: the node handle, loops
and their states, the I/O executor, stores, claims, recordings, the carried log patch, `sendspin` onto the type
interface) and the part of slice 3d that is a primitive's (claims swapped make-before-break, an in-place change that
goes on on a loop, a sink paused while a library is swapped, `sendspin` and the RTP sender changing in place). The
node lifecycle itself (states, operations, holds, the code swap, reshape, companions) is the framework page's sections
6 and 9.6; the two contexts (`ConfigureOf`, `NodeOf`) and `Configured` are declared whole in its 6.10, members of this
page's included, and this page is the home of those members' rules; section 3.3 draws the line. Slices 5 and 6 are
`docs/design/control-plane.md` (revision 5, rewritten in the same change); section 14 is the boundary, and 6.13 is the
one home of `TypeSpec`'s member list for every page.

[V] = verified in the file named, as of `3adc02b` (the code is unchanged since `f677a50`); [U] = unverified, with the
step or test that checks it (section 19). Files of the pinned sendspin-cpp tree (`7be2278`, `.cache/deps/sendspin-src`)
are cited by their path in it; PipeWire 1.4.2's source as `pipewire-1.4.2/<path>`.

## 1. The owner's words

| # | The owner's words | Date, where | What it means here |
|---|---|---|---|
| R1 | "what are the core primitives that ear should host so that new nodes can be added simply and only require defining the truly custom bits that its node type demands" | 2026-11-07, chat | The primitives sendspin wrote for itself become the framework's; a type writes its protocol |
| R9 | "Nodes should not have direct access to stdout or stderr. The core framework should expose the necessary primitives for events, diagnostics, and logging. These should be real-time safe for any audio loops that need them, and there can be non real-time bases for the rest of the system." | 2026-11-07, chat (gate, Q1) | A node's threads log through its handle; the library's own lines are attributed to the node (section 9) |
| Q5 | "Strands, dedicated on request" | 2026-11-07, chat (gate) | Loops (6.4) |
| Q7 | "Pipe into a feed first" | 2026-11-07, chat (gate) | The paced source of slice 9 builds on the source writer's contract (6.5) |
| P1 | "3 seems like a standalone unit, while 5 and 6 need to be designed together." | 2026-11-08, chat | This page stands alone; it fixes only the seams slice 5 extends (section 14) |
| P2 | "Let's start the design and architecture phase for both. I want a complete proposal or a design doc before implementation. The goal is high UX and DX" | 2026-11-08, chat | DX is what a type author writes (section 4); UX is what an operator sees (section 8) |
| R11 | "a Run should not be bounded by config changes. I want config and graph changes to be supported at runtime while audio is running" | 2026-11-08, chat (framework page R11) | Every primitive here survives an in-place change: loops and their states, stores, the node handle and unchanged claims are the node's configuration's, not the settings' (6.1) |
| R12 | "the start+build model seems overly simplistic. I think each node deserves a dedicated state machine, and hooks for each viable state. At a minimum, there should be setup+build+start" | 2026-11-08, chat (framework page R12) | Revision 3 read it as `setup`, `build`, `start`; R19 and R23 replace that with `configure`, `start()`, `stop()` (6.1) |
| R13 | "mailboxes could be supportable in either mode. But why must mailboxes be built before the run object exists? Is this a side effect of the naive node state machine model I described?" | 2026-11-08, chat (framework page R13) | Yes; revision 2's kept values go (3.1) |
| R14 | "G4 is asking the wrong question. The node audio threads should be concerned with reading inputs and writing outputs. The framework should provide the necessary primitives to connect inputs/outputs across nodes AND anytime a node needs to cross inputs/outputs to non-realtime code (eg pipe outputs through a mailbox?)" | 2026-11-08, chat (framework page R14) | Revision 2's feeds and handoffs, which the code touched, go; a type's threads meet audio only as a companion's writer or consumer (6.5, 6.6) |
| R15 | G3: "(a)" | 2026-11-08, chat (this page's G3) | A store, recording path or port held by a leaked configuration is refused with a detail saying the node starts when that run closes; the late close starts it (6.9, 6.10) |
| R16 | "I like the basic shape of the state machine proposal. And most importantly: it should be per-node. Some node stopping/reconfiguring/failing does not effect the graph globally" | 2026-11-08, chat (framework page R16) | Every primitive is per node except the I/O executor and the strand pool, which are shared and bounded (6.4, 6.7) |
| R17 | "I want the node to control whether settings can be changed in place, with the framework supporting it. For example, changing a network port on a node is something that a node can handle on its own — even if it must output null audio frames for a few seconds. But if reconfiguring changes the shape of the input/output bus, then that might not be fully transparent" | 2026-11-08, chat (framework page R17) | A port change is the worked example: claims make-before-break (6.9), a continuation on the node's loop (6.2), `sendspin`'s client swapped on its worker (4.2) |
| R18 | "crossing to a non-rat [non-realtime] thread could be managed by allowing a node to internally pipe its inputs/outputs to a hidden node, to maintain the integrity and consistency of the processing graph" | 2026-11-08, chat (framework page R18) | Sources and sinks are companions (framework page 9.6); this page owns only what a type's threads see of them (6.5, 6.6) |
| R19 | "Shouldn't that all be part of the build stage? How is build different from [re]configure?" | 2026-11-08, chat (framework page R19) | One `configure` acquires and constructs; each primitive here is asked for, or reached by handle, inside it, and readied by the framework (6.1) |
| R20, R21 | "A “run” can’t fail — only a node can fail. We should drop the “run” concept entirely and work with “graph” and “node” concepts. The graph is always running, while nodes can be added to it, configured, wired, start/stop all synchronously within the graph runtime"; "I meant “asynchronously”" | 2026-11-08, chat (framework page R20, R21) | The run handle is the node handle (5); a primitive lives as long as the node's configuration, an internal word; what a closing thread says late is dropped by a generation no type sees |
| R23 | Exactly three hooks: `configure(old, new)`, `start()`, `stop()` | 2026-11-08, chat (framework page R23) | No `setup`, `build` or `close`: a loop's state is destroyed on its own loop, after the goodbye `stop()` posted (6.4, 6.10) |
| R25 to R28 | On the framework page's F1 and F2: "“Disabled” would be a better term"; "Keeps everything"; "Both words"; "Only disabled in file" | 2026-11-08, chat (framework page R25 to R28) | Two resting states: `stopped` keeps every primitive (claims, stores, recordings, loops, companions), `disabled` releases every one by the full teardown, with the block kept (6.1, 6.10) |
| R24 | "too technical for me to properly answer" (the framework page's A4); the owner is asked only what an operator or a type author sees | 2026-11-08, chat (framework page R24) | Hooks reach loops only by typed posts (D-A4); this page's gates are decided by the designer (18.3) |

The owner's answers to revision 2's gate questions, 2026-11-08, stand: **G1** (a), a new `ear_runtime` package; **G14**
(a), loops declared in the spec as handles (each now naming its state type); **G15** (a), resources reached by
handle, not a recipe (now inside `configure`, R19); **G17** (a), the handle reached from the framework's context (now
`node.handle()`, R20); **G3** (a), R15 above. **G2** (a mailbox as a kept value) and
**G4** (a handoff whose consumer is attached late) are superseded by R13 and R14 (framework page 14.1, Q15 and Q16).
Design rules 1 to 3 of `pipewire-graph.md` hold, as do the framework page's I1 to I15.

## 2. The problem, measured

What remains for slice 3c once slices 3a (the lifecycle) and 3b (companions) are built, as the framework page's 13.2
plans them. Today's code is [V]; what 3a and 3b leave is the framework page's plan.

| Seam | Where today | After 3a and 3b |
|---|---|---|
| `sendspin` derives from `NodeTypeOf` and writes its own `NodeConfig` | `sendspin_type.cpp:72-130` | still: 3a makes it a class with `configure`, `start()` and `stop()`, its threads still its own (framework page 13.1, 13.2) |
| `NodeFolders` and `makeSendspinNodeType(const NodeFolders&)` | `node_folders.hpp:12-18`, `sendspin_type.cpp:135-144` | still: its key files and the boot sweep |
| A node's way out is `InsidesEvents` through `Channel` | `node_type.hpp:171-180`, `node_host.cpp:168-226` | still, reached as `NodeOf::handle()` (framework page 13.2) |
| Claims live in sendspin's type, across its nodes only | `port_claims.hpp:12`, `sendspin_type.cpp:83-92` | still; `rtp` claims nothing, nothing claims a recording path |
| A leaked run keeps its port or file while the next run starts | `node_host.cpp:446-464` | still |
| `file` opens its recording on the main loop and writes the WAV header there | `node_host.cpp:126-141`, `file.cpp:65-79` | the open still in `configure` on the main loop (3a moves the call, not the I/O) |
| `notYetInHarness = {"sendspin"}` | `test/nodes/all_types.hpp:71-74` | still |
| sendspin's threads: the worker (a 10 ms `poll()`), the notifier, their stop `EventFd` | `worker.cpp:93`, `200`, `278-299`, `309-323` | still its own `std::thread`s, started by `start()` |
| Its key file: named, 0600, read, trusted, created, swept at boot | `key_store.cpp:31-97`, `sendspin_type.cpp:137-144` | still its own |
| Note pacing (32, then one a second) | `note_gate.cpp:11-12` | still its own |
| The library's log lines are unattributed `stderr` records | `worker.cpp:218`; sendspin-cpp `src/platform/logging.h:56-63` | still |
| Every settings change restarts the node: a `sendspin` port or name change closes the connection and re-reads the key file | `desired_graph.cpp:159-160`, `node_host.cpp:79-87` | 3d adds the reconfigure path; what `sendspin` needs to use it is here |

What 3a and 3b remove from revision 2's list [V for today's code]: `Feed` (`feed.hpp`, `feed.cpp`, 109 lines), the two `Mailbox`
members and the capture anchor (`worker.cpp:360-361`, `sendspin_code.cpp:25-26`), the bell (`worker.cpp:358`),
`SendspinCode` (`sendspin_code.cpp`, 46 lines) and the handoff to the server (`sendspin_type.cpp:100`) become a source
and a sink companion and their writer and consumer (framework page 9.6). Revision 2 designed kept values, a feed and a
late handoff consumer for exactly these; they go (3.1).

Three facts shape what is left:

- **The instance exists before the code drives** (framework page T2, T5). So nothing a type shares has to be made
  before it; every primitive here is either the framework's (loops, stores, claims, the I/O executor) or the
  instance's or a loop state's own.
- **`ear_core` may not touch threads, files or sockets** (AGENTS.md, Architecture). Loops, stores, the probe and the I/O
  thread are interfaces in core and adapters in `ear_runtime` (G1).
- **A node's configuration outlives an in-place change, a stop and a start** (framework page 6.2, 6.6). A primitive
  bound to it (a loop and its state, a store, the node handle) must be right for every settings value the node will take, and a primitive bound to a setting (a claim on a port, a
  recording on a path) must be replaceable make-before-break.

## 3. Decisions, and where this page changes the framework page

In one paragraph: a type's spec declares its **stores** and **loops** as compile-time handles beside its banks, each
loop with the type of its **state**. Its `configure` asks for **claims** and **recordings** (and the companions of the
framework page), takes store handles, and gives each loop its state. The framework judges claims first, then opens
stores and recordings on its **I/O executor**, then starts loops; until then everything posted or armed from
`configure` is staged, and it runs, in order, once the node is configured. `start()` and `stop()` post to the node's
loops; a hook reaches a loop's state only by a post typed by the loop's handle. A node keeps its loops, their states, its
stores and its **node handle** through every in-place change, stop and start; an in-place change replaces claims and
recordings make-before-break and may go on on one of the node's loops once its new acquisitions are ready
(`cx.whenReady`). A type's threads meet audio only as a source's writer (one at a time, closed at teardown step 1) or a
sink's consumer (begun before the node is configured, paused while the type swaps what it writes into). A leaked
configuration keeps listening, and its late close starts the nodes its claims refused (R15). Implementations live in
`ear_runtime`. `sendspin` moves onto `TypeSpec` and `TypeHarness`, its worker a loop with a state, and changes its name,
port and server in place by swapping its library client on that loop.

### 3.1 What revisions 5 and 6 make unnecessary

| Revision 2 or 3 had | Why it is gone |
|---|---|
| `cx.keep<T>()`, `ready.kept<T>()`, `KeptValue` (revision 2) | Kept values were memory made before the run object because the code drove first (R13). The instance or a loop's state owns what it shares with the code (framework page 6.5, I13) |
| `ear/core/mailbox.hpp` as this page's (revision 2) | The framework page's 6.5 moves `Mailbox<T>` into core as a plain class an instance holds; it is not a primitive of this page and no built type needs it after 3b |
| The feed (revision 2: `cx.feed`, `FeedReader`, `FeedWriter`, `RingFeeds`, its latency declaration) | A source companion replaces it (framework page 9.6). Its writer's closing contract stays here, its home (6.5) |
| The handoff with a late consumer (revision 2: `cx.handoff`, `ready.attach`, `attached(skipped)`) | A sink's consumer is a member of a loop's state or given whole, named in `configure` (`consumedBy`, framework page 6.10), and the sink is armed in the invoke that drives the code and disarmed in the one that stops it (framework page 6.5, 9.6, I4; R14) |
| `RunCode::toHandoff` | A type that only takes audio out passes its bank to an inside bank with a sink (`passThrough`), so the framework writes the ring |
| `wake::named`, `cx.wake`, `ready.wake` for types | No type rings a wake after 3b: the source companion rings its own played wake. `Wake` and `Loop::onWake` stay, for the framework's wakes (6.3; G23) |
| Revision 3's `setup`, `build`, the run object's `start` and `close`; `SetupOf`, `BuildOf`, `Built`, `ReconfigureOf`, `Reconfigured`, `ReconfigureDone` | R19, R23: one `configure` (on creation and in place), `start()`, `stop()`; `ConfigureOf`, `NodeOf`, `Configured`, `ConfigureDone` (framework page 6.10). Staged loop work is released when the node is configured (6.4) |
| The run handle (revision 3's section 5) and `generation()` as a record's `run` | R20: the node handle (5); the generation is internal (framework page I6) |
| The run object's `close(closed)` and its source contract ("destroy every source before `closed`") | A loop's state is destroyed on its own loop at teardown step 4, after the work already posted; its destructor destroys its sources there (6.4, 6.10) |
| A claim held by a failed run that was not torn down | A failed node is torn down at once (framework page 6.2, R20), so its claims are released |

### 3.2 Where this page refines the framework page

Each refinement is a module-level detail this page is the home of; the framework page keeps the contract it states.

| Framework page | Refinement | Here |
|---|---|---|
| 6.4, readiness | Its steps 1 to 3 and 4' are this page's (claims, stores and recordings, loops; every sink consumer has begun, 5 s); the framework page lists them and cites this page | 6.1, 6.6 |
| 6.4, "work posted from `configure` runs once configured" | Sources armed and work posted from `configure`, or from a loop state's constructor, are staged; released in the order made at T3 | 6.4 |
| 6.6, `configure`'s in-place contract | `cx.whenReady(loop, work)`: the change's continuation, posted to one of the node's loops once every new acquisition is ready, with the loop's state and the `ConfigureDone` token; the one way to finish later | 6.2 |
| 6.6, step 3 and "Break before make" | A replacement asking a claim key its node already holds is granted at once, not probed; an RTP receiver whose port is unchanged is replaced break-first by the framework, without the type asking, behind the framework's own acknowledged hold | 6.9 (G22) |
| 6.6, step 5 (deadline) | A claim wait inside an in-place change counts against the change's deadline, not its own 15 s; the fallback restart waits afresh | 6.9 |
| 6.6, the built-in types' table, `sendspin` row | This page is the row's home. All three rows in place, the listening/dialling switch included: sendspin-cpp's configuration is fixed when the client is made (`include/sendspin/client.h:333-335` [V]), so a name or port change needs a new client anyway, and a new client may as well dial; the node's configuration, key store, loops and companions stay; the connection and the audio do not | 4.2 (G19) |
| 9.6, `SinkConsumer` | This page declares `SinkConsumer` (with `begin()`, once, on the sink's thread, before the node is configured) and `SinkControl` (`pause`, `resume`) whole, their one home; the framework page keeps the sink's realtime contract | 6.6 (G16, G21) |
| 9.6, the source writer | One writer at a time, handed over only by a join or a loop post; its closing contract | 6.5 |
| 7.3, Wake row | Types do not ask for wakes yet | 6.3 (G23) |
| 7.3, Recording row | A deferred kind: claimed at step 1, opened at step 2, its descriptor usable once configured | 6.11 |
| 8.8's erratum | The node handle and its node-facing `log` are slice 3c's; the boot sweep's findings stay `log` records from the type's name until slice 5 | 5, 6.8 |
| 12 ("the loop pool, eventfds and stores are behind interfaces, in `ear_runtime`") | `ear_runtime` links `ear_folder`; it exists from 3b (the sink's thread), and 3c adds loops, the I/O thread, the probe and stores to it | 11 |

### 3.3 The boundary with the lifecycle

| The lifecycle's (framework page 6, 6.10 and 9.6) | This page's |
|---|---|
| The operations, the states, the transitions T1 to T18, what arrives mid-transition and supersession | What each primitive does in each state (6.1's table) |
| `ConfigureOf`, `NodeOf`, `Configured`, declared whole in framework page 6.10 (every member, this page's included), and their companion calls (`soundCard`, `rtpReceiver`, `source`, `sink`, `consumedBy`) | The rules of its members: claims, recordings, stores, loops and their states, the handle, `whenReady` and `ConfigureDone`, the source writer and `SinkControl` (5, 6.1, 6.2, 6.4, 6.5, 6.6, 6.8, 6.9, 6.11) |
| Readiness's order and step 3' (companions); T5's invoke and `start()` | Readiness steps 1 to 3 and 4' (6.1) |
| Holds and their acknowledgement, the code swap, reshape, the plan's outcomes, the in-place deadline (`Lifecycle`) | — (6.13 places the `.lifecycle` member) |
| I5's steps as a contract; companions at steps 2 and 3 | Each primitive's place in every step, full and partial (6.10); the leaked-configuration record and late close (6.10) |
| The ring companions' realtime side and their thread | The writer's and consumer's contracts as a type's threads see them (6.5, 6.6) |
| The harness's class and operations (framework page 7.6) | The harness's loop, store, recording, claim and handle fakes and its clock (section 10) |

## 4. Journeys: what a type author writes

### 4.1 A type with its own threads: sendspin

**Before** (today, abridged; every line cited is [V]). The type holds a claims map across its nodes; `start` claims and
probes, builds the whole run object (which reads the key file and starts two threads), acquires the handoff, declares
the feed's latency and returns the code with a recipe that only hands the object over (`sendspin_type.cpp:83-108`):

```cpp
StartResult start(StartContext& context) const override {
	if (!settings_.server) { /* claims_->claim(port), HeldPort, probeListenPort(port) */ }
	auto run = std::make_unique<SendspinRun>(folder_, node_, settings_, context, std::move(port)); // threads start
	RtWriter& toServer = context.acquire(HandoffRequest{"to", rate / 2, &run->toServer()});
	context.declareLatency("from", Latency{0, feedFramesFor(context.format())});
	auto code = run->code(rate, toServer);                 // reads the run's feed, mailboxes and bell
	return StartResult{std::move(code), [run = std::move(run)](Ready&) mutable { return std::move(run); }};
}
// worker.cpp:189-215: the worker thread opens the key file, makes the client, starts the notifier thread, then polls
// every 10 ms until stop. makeSendspinNodeType(const NodeFolders&) sweeps the folder and writes to the boot log.
```

**After** (slices 3a to 3d). The spec declares the banks, the key file and the two loops, the worker with its state
type. `configure` acquires and constructs: it gives the worker its state and names the sink's consumer; `start()` and
`stop()` post to the worker; an in-place `configure` hands a change to the worker. No line carries a resource from one
hook to another, and no hook touches the worker's state but by a post (framework page I14).

```cpp
// src/nodes/sendspin/sendspin_spec.hpp
namespace ear::nodes::sendspin {
namespace rows {   // today's rows (settings.hpp), each marked in place (6.6 of the framework page)
inline constexpr auto server = setting::optional("server", setting::text<ServerUrl::parse>("ws://<host>[:<port>]/<path>")).inPlace();
inline constexpr auto port = setting::defaulted("port", setting::number(1, 65535), 8928).onlyWithout(server).inPlace();
inline constexpr auto name = setting::optional("name", setting::text<parseName>("1 to 64 bytes of UTF-8 ...")).inPlace();
}
inline constexpr auto to = bank::input<"to">;
inline constexpr auto from = bank::output<"from">;
inline constexpr auto toServer = bank::insideOutput<"to_server">;     // its sink companion joins here
inline constexpr auto fromServer = bank::insideInput<"from_server">;  // its source companion joins here
/// sendspin-<node>.key in the graph file folder: mode 0600, at most 64 KiB, created empty, trusted only as a key record.
inline constexpr auto keyFile =
	store::file<"key">({.mode = 0600, .maxBytes = 64 * 1024, .initial = &emptyKeyRecord, .distrust = &whyNotKeyRecord});
class Worker;   // worker.hpp: the library's client, pairing, the time filter, mDNS; lives on the worker loop
inline constexpr auto worker = loop::dedicated<"sendspin", Worker>;   // the library's main loop: start, loop(), callbacks
inline constexpr auto notifier = loop::dedicated<"notify">;           // no state: the worker arms it; a late notify
                                                                      // costs a hard sync, so it is apart
inline constexpr TypeSpec spec{.settings = SettingsTable<rows::server, rows::port, rows::name>{"sendspin"},
                               .banks = Banks<to, from, toServer, fromServer>{},
                               .stores = Stores<keyFile>{}, .loops = Loops<worker, notifier>{}};
using Checked = CheckedOf<spec>;

class Sendspin {   // the node's instance (framework page 6.4): main-loop state only
public:
	static Members<spec> ports(const Checked&) {
		return {to(members("left", "right")), from(members("left", "right")),
		        toServer(members("left", "right")), fromServer(members("left", "right"))};
	}
	explicit Sendspin(NodeOf<spec>& node) : node_(node) {}
	Configured configure(ConfigureOf<spec>& cx, const Checked* old, const Checked& now);
	void start() { node_.post(worker, [](Worker& w) { w.begin(); }); }   // make the client, start it, listen or dial
	void stop() { node_.post(worker, [](Worker& w) { w.end(); }); }      // goodbye; the client stopped and destroyed
private:
	Configured changeInPlace(ConfigureOf<spec>& cx, const Checked& old, const Checked& now);   // 4.2
	NodeOf<spec>& node_;
};
}

// src/nodes/sendspin/sendspin.cpp
Configured Sendspin::configure(ConfigureOf<spec>& cx, const Checked* old, const Checked& now) {
	if (old) return changeInPlace(cx, *old, now);
	if (!now.get<rows::server>()) cx.claim(claim::TcpListen{now.get<rows::port>()});   // judged first (6.9)
	SourceWriter& played = cx.source(fromServer, {.frames = feedFramesFor(cx.format())});   // a companion (9.6)
	cx.loop(worker, std::make_unique<Worker>(WorkerParts{
		.handle = node_.handle(), .keys = cx.store(keyFile), .played = played,
		.self = cx.executor(worker), .notifier = cx.executor(notifier), .settings = settingsFrom(cx.node(), now)}));
	cx.sink(toServer, {.frames = cx.format().rate / 2}).consumedBy(worker, &Worker::toServer);   // begun before running
	cx.use(passThrough({bankPair(to, toServer), bankPair(fromServer, from)}));
	return Configured::applied();
}
std::unique_ptr<const NodeType> makeSendspinNodeType() { return makeNodeType<sendspin::spec, sendspin::Sendspin>(); }
```

The worker's state takes what it uses in its constructor (on the main loop, inside `configure`) and arms nothing there:
the store is not open yet, and the loop not started. Its work runs on the worker loop once the node is configured
(6.4); `begin()` reads the key record, makes the client and arms the tick; `end()` is the goodbye. Its destructor runs
on the worker loop when the node's configuration is torn down (framework page I5 step 4) and destroys whatever is left:

```cpp
void Worker::begin() {   // on the worker loop, after start()
	KeyRecord record = KeyRecord::parseTrusted(keys_.text());   // the store's rule already trusted the text
	startClient(std::move(record));                             // make, start, listen or dial
	tick_ = self_.every(std::chrono::milliseconds{10}, [this] { tick(); });            // client.loop(), watch, mDNS
	notifier_.post([this] { notified_ = notifier_.onWake(played_.playedWake(), [this] { notify(); }); });
}
void Worker::end() {     // on the worker loop, after stop(); also queued ahead of the destructor at removal
	notifier_.post([this] {
		notified_.reset();                  // on its own loop: no notify runs after this line
		self_.post([this] { tick_.reset(); stopClient(); });   // client->stop(): goodbye, its threads joined
	});
}
Worker::~Worker() { /* on the worker loop: a client still alive is stopped; sources left are destroyed (6.4) */ }
// The library's sync thread writes the source companion (framework page 9.6), as it writes the feed today:
std::size_t Worker::on_audio_write(std::uint8_t* data, std::size_t length, std::uint32_t timeoutMs) {
	if (!streamAsked_.load(std::memory_order_acquire)) {
		played_.idle(std::chrono::milliseconds{timeoutMs});   // sleeps its patience, before or after the close (6.5)
		return 0;
	}
	/* as today: convert, then played_.write(scratch_, frames, std::chrono::milliseconds{timeoutMs}) */
}
```

What the author no longer writes: a thread, a poll loop, an eventfd, a mailbox, a feed, a capture anchor, realtime code,
note pacing, a claims map, a probe, a `HeldPort`, a key file's open, mode or sweep, a `NodeConfig`, a `NodeTypeOf`, a
latency declaration, a folder parameter, a close hook, or a lambda that carries resources between hooks. What stays:
settings, the protocol callbacks, the key record's format, mDNS, pairing notes (until slice 5), and the client swap of
4.2.

### 4.2 Changing in place: sendspin changes its port

The owner's example (R17): a network port changed while audio runs, which the node handles on its own. Scenario: node
`kitchen` listens on 8928 and Music Assistant streams to it; the operator sets `port 8929;` in the editor.

What the type writes (called only when every changed key is `.inPlace()`, framework page 6.6):

```cpp
Configured Sendspin::changeInPlace(ConfigureOf<spec>& cx, const Checked& old, const Checked& now) {
	SendspinSettings next = settingsFrom(cx.node(), now);
	if (old.get<rows::server>() && now.get<rows::server>() && settingsFrom(cx.node(), old).name == next.name) {
		cx.post(worker, [url = *next.server](Worker& w) { w.redial(url); });   // connect_to: no new client
		return Configured::applied();
	}
	if (!old.get<rows::server>()) cx.unclaim(claim::TcpListen{old.get<rows::port>()});   // released at the end
	if (!now.get<rows::server>()) cx.claim(claim::TcpListen{now.get<rows::port>()});     // judged now (6.9)
	return cx.whenReady(worker, [next = std::move(next)](Worker& w, ConfigureDone done) mutable {   // 6.2
		w.swapClient(std::move(next), std::move(done));
	});
}
```

The worker's state is reached only on its loop (`cx.post`, `cx.whenReady`; framework page 6.10), typed by the loop's
handle, so the hook cannot race the worker on the main loop. A redial is `connect_to` on the running client
(`client.h:389` [V]): it keeps the client, but drops the connection to the old server, so the stream ends too. A stopped
node takes the change the same way: its worker records the new settings, and the next `begin()` uses them.

`swapClient` runs on the worker, the library's main loop, which `start()` and `stop()` require
(`include/sendspin/client.h:337`, `362`) [V]. It is `end()`, then `startClient` with the new settings:

1. Posts to the notifier, which destroys its `onWake` source (so no `notify_audio_played` reaches the old client's
   player), and back to the worker.
2. `toServer_.pause(worker_, ...)`: the sink's thread finishes any `consume` in flight and then discards chunks until
   `resume`, and posts back once no `consume` can run (6.6), so `write_audio` never reaches a client being destroyed.
3. Says goodbye on mDNS (`mdns_goodbye_multicast`, `advertiser.cpp:257`), then `client->stop()`: goodbye to the server,
   the library's threads joined, `on_stream_end` delivered (`client.h:339-346`) [V], which clears the source; the
   client destroyed. The old sync thread, the source's writer, is joined, so the new one may write (6.5).
4. Makes the new client over the same key store, so the identity and pairings stay (U11), with `log_context` the same
   node handle (section 9); `start()`; on `false`, `done.fail("the Sendspin client did not start")`.
5. Re-arms the notifier's `onWake`, `toServer_.resume()`, opens the advertiser on the new port, and calls `done.done()`.

What the framework does around it (framework page 6.6 is the home of the steps; these are the primitives' parts):

| When | The framework | The operator sees |
|---|---|---|
| The commit | the plan's outcome is `reconfigure` (every changed key `.inPlace()`, ports unchanged); calls `configure(cx, &old, new)` on the main loop | `node kitchen reconfiguring: port` |
| The hook returns | judges `tcp:8929` (6.9): free, so granted and probed; no hold asked: the source plays what it holds until the stream ends, then fades out (framework page 9.6) | — |
| Every new acquisition ready | posts the continuation to the worker with the token (6.2) | notes `disconnected` and `stream-ended` from the old client |
| `done.done()` | releases `tcp:8928`, last; the node is `running` | notes `listening 8929 as ear-kitchen-<id>`, `id <same client id>`; `node kitchen running` |
| Music Assistant finds it again | — | `connected "Music Assistant" paired` |

**What in place buys here, and what it does not.** The audio stops either way: the old client says goodbye, the
stream ends, and Music Assistant reconnects to the new port (or the new server). Against a restart, in place keeps the
node's configuration (its instance, its worker's state, its status, its loops and companions), the identity without reading the key file again (no disk
I/O, and no failure from a file that changed meanwhile), and both ports claimed for a moment so no other node takes
the old one mid-change; the operator sees `reconfiguring: port` instead of `stopping: restarting (port)`. It costs `swapClient`,
the sink's pause (6.6) and two unknowns (U11, U12). G19 is decided by the designer for in place (18.3).

Its failures, each a fallback to a restart with the new settings (framework page 6.6, D-B4):

- **8929 held by another running node**: refused at once; the hook's continuation is destroyed unrun; the restart's
  `configure` claims 8929 again and is refused: `node kitchen stopping: restarting (claim refused)`, then `failed start-failed: cannot
  listen on port 8929: held by node porch`.
- **8929 held by a node still closing**: the claim waits with `ear.claim-wait`, against the change's 10 s deadline
  (6.9); past it the restart's claim waits afresh. A newer edit meanwhile (the operator types 8930) supersedes the
  change before its continuation is posted (framework page T18): the wait for 8929 is dropped and the hook is
  called again for 8930.
- **The new client does not start**: `done.fail(...)`, then the restart; a second failure is `failed` and recovery
  (slice 8).
- **Stop or removal during the swap**: a continuation not yet posted is destroyed unrun; one already posted is the node's
  own work, runs, and the goodbye queued behind it stops whichever client exists (6.10).


### 4.3 A type that needs a store, a claim and a consumer

The example is an RTP *sender* (`rtp.md` section 9 lists it as not built), kept small: it sends its input bank as L24
to a unicast address from a fixed source port, which no two nodes may share (a claim), and keeps its SSRC across
restarts so a receiver does not see a new stream each time (a store). The address and port change in place; the source
port restarts. It illustrates the interface; this page does not ask for the type.

```cpp
// src/nodes/rtpsend/rtpsend_spec.hpp
namespace ear::nodes::rtpsend {
std::string newSsrc();                                              // eight random hex digits
std::optional<std::string> whyNotSsrc(std::string_view text);      // "malformed: ..." unless eight hex digits
namespace rows {
inline constexpr auto address = setting::required("address", setting::text<parseIpv4>("an IPv4 address")).inPlace();
inline constexpr auto port = setting::required("port", setting::number(1, 65535)).inPlace();
inline constexpr auto sourcePort = setting::defaulted("sourcePort", setting::number(1, 65535), 5006);
inline constexpr auto channels = setting::required("channels", setting::number(1, 8));
}
inline constexpr auto send = bank::input<"send">;
inline constexpr auto wire = bank::insideOutput<"wire">;            // its sink companion joins here
inline constexpr auto ssrcFile =
	store::file<"ssrc">({.mode = 0644, .maxBytes = 64, .initial = &newSsrc, .distrust = &whyNotSsrc});
class Sender;
inline constexpr auto control = loop::strand<"control", Sender>;    // the sender lives on its control strand
inline constexpr TypeSpec spec{.settings = SettingsTable<rows::address, rows::port, rows::sourcePort, rows::channels>{"rtpsend"},
                               .banks = Banks<send, wire>{}, .stores = Stores<ssrcFile>{}, .loops = Loops<control>{}};

/// The control strand's state, and the sink's consumer. One thread owns the socket at a time: the sink's (begin,
/// consume, finish), or the control strand while the sink is paused (retarget). The pause's post is the hand-over (6.6),
/// so nothing else synchronises.
class Sender final : public SinkConsumer {
public:
	Sender(NodeHandle& handle, StoreHandle& ssrc, SinkControl& sink, Loop& control, Ipv4 address, std::uint16_t port,
	       std::uint16_t sourcePort);
	void begin() override;                     // reads the SSRC; socket, bind sourcePort, connect; failure: lost(...)
	void consume(const Chunk& chunk) override; // RTP timestamp from chunk.position; RTCP every 5 s of audio
	void finish() override;                    // BYE, close the socket
	void retarget(Ipv4 address, std::uint16_t port, ConfigureDone done) {   // on the control strand
		sink_.pause(control_, [this, address, port, done = std::move(done)]() mutable {
			reconnect(address, port);            // connect(2) on a UDP socket: never blocks
			sink_.resume();
			done.done();
		});
	}
private:
	NodeHandle& handle_;
	StoreHandle& ssrc_;
	SinkControl& sink_;
	Loop& control_;
	int socket_ = -1;
};

class RtpSend {   // the node's instance: nothing of its own to keep
public:
	static Members<spec> ports(const CheckedOf<spec>& s) {
		const auto n = s.get<rows::channels>();
		return {send(members(n)), wire(members(n))};
	}
	explicit RtpSend(NodeOf<spec>& node) : node_(node) {}
	Configured configure(ConfigureOf<spec>& cx, const CheckedOf<spec>* old, const CheckedOf<spec>& now) {
		const auto address = parseIpv4(now.get<rows::address>());
		const auto port = static_cast<std::uint16_t>(now.get<rows::port>());
		if (old) return cx.whenReady(control, [address, port](Sender& s, ConfigureDone done) mutable {
			s.retarget(address, port, std::move(done));
		});
		cx.claim(claim::Udp{now.get<rows::sourcePort>()});     // takes the checked number; outside 1-65535: logic_error
		SinkRequest& sink = cx.sink(wire, {.frames = cx.format().rate / 10});
		cx.loop(control, std::make_unique<Sender>(node_.handle(), cx.store(ssrcFile), sink.control(),
		                                          cx.executor(control), address, port,
		                                          static_cast<std::uint16_t>(now.get<rows::sourcePort>())));
		sink.consumedBy(control);                              // the loop's state is itself the consumer
		cx.use(passThrough({bankPair(send, wire)}));
		return Configured::applied();
	}
private:
	NodeOf<spec>& node_;
};
}
```

Neither `start()` nor `stop()` is
written: the sink's arming (framework page T5, T6) starts and stops the sending, and the socket stays open while
stopped.

The SSRC file is `rtpsend-<node>.ssrc` (I9), created with `newSsrc()` on first configure, narrowed to 0644 at boot and
at each open; a corrupt one fails the node `start-failed: rtpsend-<node>.ssrc: malformed: ...` and is not replaced. A
second node on the same source port fails `start-failed: cannot bind UDP port 5006: held by node <n>`; removing one node
and adding another on that port in one plan works, the new one starting once the old one let go (I8), with `node <new>
ear.claim-wait: udp:5006 held by node <old>, which is closing` while it waits. A changed address loses the audio of the
pause (a few milliseconds, counted as paused frames, 6.6) and keeps the SSRC; the receiver sees a jump in RTP timestamp,
which RTP allows.

## 5. The node handle

**Decision: the node handle is slice 3c's** (the framework page's 13.1 and 13.2 put it there: 3a keeps `Channel`,
reached as `NodeOf::handle()`, and 3c replaces it). Slice 5 builds over it (`Report<spec>`, control-plane.md) and
beside it (the realtime ring the framework makes with each configuration). Reasons: every loop handler needs a
generation-checked way out; the carried log patch (section 9) needs a node log that carries `node`, which `LogEntry`
already holds (`log.hpp:17-25`) [V]; one name avoids a second rename in slice 5. The generation it carries is internal
(framework page I6): records and the wire never show it, and `LogEntry::run` (`log.hpp:23`) goes.

```cpp
// ear/core/node_handle.hpp: replaces InsidesEvents (node_type.hpp:171-180)
/// A node's way out, from any of its threads; never blocks. Shared-owned by the framework, so it outlives every thread
/// that can reach it, a leaked configuration's included. What it carries lands only on the configuration that made it
/// (framework page I6). An in-place change keeps it; a restart makes a new one.
class NodeHandle {
public:
	virtual const NodeName& node() const noexcept = 0;
	/// A fact: the node cannot go on (framework page T14). The node fails `lost` with `detail` once it has reached
	/// `running` or `stopped`, and `start-failed` before that (from a consumer's `begin`, or work run once configured);
	/// the framework page's 6.2 is the codes' home, and recovery's first wait follows the phase (its 8.6). During an
	/// in-place change it fails the node (T14: the node itself is lost, not a replacement; T11 is for the token's `fail`).
	/// Ignored once teardown began.
	virtual void lost(std::string detail) = 0;
	/// Free text for people (framework page 8.8). The framework sets `node`; an empty `source` is the type's name.
	/// Straight to the log sink (any thread, never blocks), paced per node by the encoder as today. Dropped once the
	/// handle is detached.
	virtual void log(LogEntry entry) = 0;
	void debug(std::string text) { log({.level = LogLevel::debug, .text = std::move(text)}); }
	void info(std::string text) { log({.level = LogLevel::info, .text = std::move(text)}); }
	void warn(std::string text) { log({.level = LogLevel::warn, .text = std::move(text)}); }
	void error(std::string text) { log({.level = LogLevel::error, .text = std::move(text)}); }
	/// Interim, until slice 5 splits notes into status and events: `node <n> says <text>`, on the main loop in order.
	/// Paced per node: 32 at once, then one a second; the next note that passes is preceded by `notes-dropped <n>`. An
	/// in-place change, a stop or a start does not reset it; a restart does. Heard until the handle is detached, so a
	/// goodbye note is heard.
	virtual void say(std::string text) = 0;

protected:
	~NodeHandle() = default;
};
```

- **Order.** `lost` and `say` cross to the main loop in order (as `Channel` does today, `node_host.cpp:173-226` [V]),
  so a note is ordered with node states. `log` goes straight to the sink and is not ordered with states, as captured
  output is not today. Its detach check is an atomic flag.
- **Pacing.** `say`'s pacing is sendspin's `NoteGate` (`note_gate.cpp:11-12`) moved into core, one gate per node;
  `lost` is never held back.
- **Where it is reached.** `node.handle()` on the instance's `NodeOf` (G17), which a type hands to its loops' states
  in `configure`; `configure` itself has nothing to tell.
- **The sink.** `NodeHost::logTo(LogSink&)`, wired in `main.cpp` to `StatusLog`, the sink captured output already uses.
- **A leaked configuration's handle** is detached from the node and re-pointed at the leaked-configuration record
  (6.10), which hears its late close; `lost`, `log` and `say` from it are dropped.

## 6. The primitives

### 6.1 Each primitive through the lifecycle

The framework page's states (6.2 there) against this page's primitives. "—" means the primitive is not touched in that
state.

`disabled` (framework page 6.2, R25 to R28) is not a column: a disabled node has no configuration, so every primitive
below is as after the teardown column (claims released, stores and recordings closed, loops' states destroyed,
companions gone), and enabling it starts again at `configuring` with nothing held over. A `stopped` node keeps them all
(R26).

| Primitive | `configuring`: `configure` | `configuring`: readiness (step) | `configured` | `running` (`start()`) and `stopped` (`stop()`) | `reconfiguring` | Teardown (I5 step; a disable runs it whole) |
|---|---|---|---|---|---|---|
| Claim (6.9) | `cx.claim(...)`; implicit for stores, recordings, RTP receivers | judged first (1), then ports probed | held | held, running or stopped | `cx.claim` judged now; `cx.unclaim` released at the commit point; unchanged ones kept | released last (5); abandoned or kept by a leak (I5a) |
| Store (6.8) | declared in the spec; `cx.store(h)`, a handle | opened on the I/O executor (2), 5 s | `text()`, `replace`, `replaceLater` on the node's loops | the same | kept | usable until step 4; freed (5) |
| Recording (6.11) | `cx.recording({path})`: a descriptor, usable once configured | claimed (1), opened (2), 5 s | its consumer's `begin` writes the header | written by its consumer while running | `cx.recording(...)` replaces it make-before-break | closed (5) |
| Loop (6.4) | declared in the spec; `cx.loop(h, state)`, `cx.executor(h)`; posts and arming staged | started (3) with its state, nothing run | staged items released, in order | `start()` and `stop()` post to it | kept; `cx.post` and `cx.whenReady(loop, ...)` | closed to others (1); runs what was posted, then destroys its state on its thread (4); freed (5) |
| Node handle (5) | `node.handle()` | — | `lost`, `log`, `say` | the same | the same | detached at 1 for facts; re-pointed at a leak |
| Source writer (6.5; a companion's) | `cx.source(bank, ...)`: the writer | the companion's (3') | written by one thread | written while the type plays | kept, or replaced by `cx.source` | closed (1); ring freed (5) |
| Sink consumer (6.6; a companion's) | `cx.sink(bank, ...)`, `consumedBy(...)` | the companion's (3'); `begin` (4') | — | armed with the code's drive (T5, T8), disarmed at a stop (T6); `consume` on the sink's thread | `pause` and `resume`; or replaced by `cx.sink` | drained, `finish` (3); ring freed (5) |
| I/O executor (6.7) | — | runs the opens (2) | runs store writes | the same | runs the replacement opens | completions after teardown are dropped |

So a primitive bound to the node's configuration (a store, a loop and its state, the handle) is made once when the node
is configured from empty and survives every in-place change, stop and start (R11, R20); a primitive bound to a setting
(a claim on a port, a recording on a path, a companion) is replaceable in place.

**This page's members of the contexts.** The framework page's 6.10 declares both contexts whole; these are the members
whose rules are this page's, with where:

| Context | Member | Rules |
|---|---|---|
| `ConfigureOf<spec>` | `claim(claim::TcpListen)`, `claim(claim::Udp)`, `unclaim(...)` | judged at readiness step 1, or now in place; released at the commit point (6.9) |
| `ConfigureOf<spec>` | `recording(RecordingRequest)` | claims `recording:<path>`; opened at step 2; its descriptor (6.11) |
| `ConfigureOf<spec>` | `store(handle)` | a handle of `Spec.stores` (static); open and trusted once configured (6.8) |
| `ConfigureOf<spec>` | `loop(handle, state)`, `executor(handle)`, `post(handle, work)` | a handle of `Spec.loops` (static); the state's type is the handle's; staged until configured (6.4) |
| `ConfigureOf<spec>` | `source(bank, shape)`, `sink(bank, shape)` | the writer (6.5); the request, its consumer and its control (6.6) |
| `ConfigureOf<spec>` | `whenReady(loop, work)` | the one later answer of an in-place change (6.2) |
| `NodeOf<spec>` | `handle()`, `post(handle, work)` | the node handle (5; G17); posts to a loop's state (6.4) |

- **Stores and loops are declared, not discovered**: the spec declares them, so every node of the type has them, the
  boot sweep knows every store before any node exists, and slice 5's `writer::loop` and `call<..., Loop>` take a loop
  handle the spec lists (control-plane.md). A declared loop with a state type must be given its state in `configure`
  on creation (else a `std::logic_error` at configure's return).
- **Look-ups that fail** at run time (a sink with no consumer at configure's return, a consumer named twice) throw
  `std::logic_error` naming the node and the handle: a type bug, found by its first harness test. Store and loop
  handles the spec lacks, and a post whose function takes another loop's state type, do not compile.
- **Every `cx.` call runs inside `configure` only**; else `std::logic_error`, as `StartContext` does today
  (`node_host.cpp:126-127`) [V]. `NodeOf`'s calls are valid for the instance's life, on the main loop.
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
  `NodeLoop`, `NodeStore` and `NodeRecording` are the framework's sides of a loop, a store and a recording; a type sees
  only `Loop`, `StoreHandle` and a descriptor.

**Readiness**, this page's steps of the framework page's 6.4, ordered by cost and side effects so that a refusal costs
nothing:

| # | What | When it fails |
|---|---|---|
| 1 | **Claims**, in the same main-loop turn as `configure` returned: each is judged (6.9). A refusal fails the node now, and nothing below is touched. A wait holds every later step. Once all are granted, ports are probed (main thread, a non-blocking bind) | `start-failed: <claim's text>` |
| 2 | **Stores and recordings**, opened on the I/O executor, one job each, in request order (5 s each) | `start-failed: <file>: <why>`; `start-failed: cannot open <path>: <why>` |
| 3 | **Loops**, once step 2 is ready: dedicated threads started, strands registered, each with its state. Threads last, so a refused store starts none | `start-failed: cannot start loop <name>: <errno text>` |
| 3' | Companions (the lifecycle's), once the block is ready and step 1 granted, in parallel with 2 and 3 | `engine-refused: <detail>` |
| **4'** | **Every sink consumer has begun**: `begin()` returned on its sink's thread (6.6), 5 s | `start-failed: <what begin passed to lost>`, or `start-failed: <bank>: consumer did not begin within 5 s` |

Then T3: the node is `configured`; the code is prepared and the work staged on its loops is released. Then, wanted
running, T5: once the block is ready, one data-loop invoke drives the code and arms the sinks (framework page I4), and
`start()` runs once it completed. **A configuration that fails before `configured`** (a claim refused, a store
distrusted, a loop that does not start, a consumer that does not begin) is torn down by I5 (6.10): its staged items are
destroyed unrun on the main loop with the loop states they would have run on; a loop that started destroys its state
on its own thread.

### 6.2 An in-place change that goes on on a loop: `whenReady`

**Purpose.** An in-place change often cannot act until its new acquisitions are ready (sendspin's new client must not
start before its port is granted and probed), and must act on the node's own thread, not the main loop (I11, I14).
`whenReady` joins the two, so a type writes neither a poll nor a second hook.

```cpp
/// An in-place change's token (framework page 6.6): move-only; done() or fail(detail) once, from any thread. Destroyed
/// without either, it fails the change ("<node>: the change was dropped"). After teardown began, both are ignored.
class ConfigureDone {
public:
	void done() &&;
	void fail(std::string detail) &&;
};
/// In ConfigureOf<Spec> (framework page 6.10), in place only: once every acquisition this call made is ready (claims
/// granted and probed, recordings open, replacement companions running and linked), posts `work(state, token)` to
/// `loop`, a handle of the spec's loops, as the node's own work on that loop's state. Returns the only "later"
/// Configured there is: configure returns it. With nothing acquired, posts at once.
template <class Loop>
Configured whenReady(Loop loop, std::move_only_function<void(typename Loop::State&, ConfigureDone)> work);
```

- **If an acquisition is refused or lost**, or the change's deadline passes before the post, `work` is destroyed unrun
  on the main loop and the change falls back to a restart (T11). Its token's destructor then does nothing.
- **If a newer change supersedes it** before the post (framework page T18), or `stop`, a removal or the `restart`
  action ends it, `work` is destroyed unrun the same way and its acquisitions are released; the post is the commit
  point after which a newer change waits.
- **Once posted, `work` is the node's own work** (6.4's intake): it runs even if a stop, a restart or a removal began
  after the post, before the goodbye that `stop()` posts behind it, so a type's goodbye always sees a whole state, old
  or new, never half a swap. Its `done` is then ignored.
- **The deadline** (framework page 6.6, step 5: 10 s by default, a type may declare up to 60 s with `.lifecycle`,
  6.13) counts from `configure`'s call, claim waits included (6.9).
- **The code.** A continuation may not swap the code or hold banks: those are `cx` calls, made in `configure`, applied
  by the framework at the commit point (framework page 6.6, step 4).
- **Harness.** `h.change(settings)` calls `configure` and readies its acquisitions as `h.add()` does; the continuation
  runs on the next `runLoops()` (section 10).
- **Why it is the only later answer.** With `whenReady` alone a token and a later answer cannot disagree, and the type
  never has to learn when its claim was granted (a callback per kind, or a poll): `whenReady` is the in-place change's
  continuation, run once everything it asked for is ready, where the type works. A type whose library finishes on its
  own thread hands the token on from `work`.


### 6.3 Wakes: the framework's, for now

**Purpose.** A thread (the realtime one included) tells a loop "look now", at most once until the loop answers. After 3b
the only wakes are the framework's: a source companion's played wake (framework page 9.6), and later the status bell
(slice 5). Types do not ask for wakes in slice 3c (G23).

```cpp
class Wake {   // ear_core
public:
	/// Any thread, the realtime one included. Never blocks, locks or allocates:
	///   if (!pending_.exchange(true, std::memory_order_acq_rel)) write(efd_, 1);
	virtual void ring() noexcept = 0;
protected:
	~Wake() = default;
};
```

- **Answered** by the loop that watches it (`Loop::onWake`, 6.4), in this order: `read(efd)` (draining it), then
  `pending.store(false, std::memory_order_release)`, then the handler. A ring that lands after the drain and before the
  clear finds `pending` still true and writes nothing, but the handler that follows sees its data; a ring after the
  clear writes the eventfd again. So no ring is lost and `pending` is never true with the eventfd empty for good. One
  watcher per wake (`std::logic_error` for a second).
- **Rung before it is watched.** A source plays from T5, before `begin()` arms `onWake`, so a wake may be pending when
  its watch is armed; arming a watch on a pending wake runs the handler once, on the loop's next turn.
- **Realtime.** One `write(2)` on a non-blocking eventfd at most, as the handoff's wake already does from `process`
  (`handoffs.cpp:54`, through `pw_loop_signal_event`) [V]. sendspin rings every cycle with data today
  (`sendspin_code.cpp:43`) [V]; gated, it writes once per notifier turn.
- **Implementation.** `EventWake` in `ear_runtime`. If 3b builds the source companion before loops exist, its played
  wake carries an interim `fd()` that sendspin's notifier thread polls, as it polls its bell today; S3c.3 removes it
  (U14).
- **Tests.** The order above, on the harness (a ring between drain and clear, by a hook) and on real threads (a stress
  test: a ringing thread and a watching loop for 10^6 rings, every ring's data seen, the loop never stuck).
- **Replaces.** `EventFd` (`event_fd.hpp`) as the bell.

### 6.4 Loops

**Purpose.** A serial executor for a node's work off the realtime thread and the main loop (Q5: strands by default,
dedicated on request), declared in the spec (G14), with its state, alive for the node's whole configuration: stops, starts and in-place
changes included.

```cpp
// ear/core/loop.hpp
namespace loop {
/// A loop of a type, declared in its spec. Name: 1 to 8 of [a-z0-9-], unique in the spec (both checked at compile time).
/// State: the type of the loop's state (framework page 6.10), or none; posts to the loop take a State&.
template <spec_detail::LoopName Name, class State = NoState> inline constexpr LoopHandle<Name, LoopKind::strand, State> strand{};
template <spec_detail::LoopName Name, class State = NoState> inline constexpr LoopHandle<Name, LoopKind::dedicated, State> dedicated{};
}
template <auto... Handles> struct Loops;   // TypeSpec's `.loops` (6.13)

/// A timer or a watch. Destroying it cancels it: its handler never runs after the destructor returns. Destroyed on its
/// loop's thread; or on the main loop while staged (from configure or a state's constructor); or on any thread once its
/// loop is closed (step 4). Anywhere else is a type bug (asserted in debug builds and in the harness).
class LoopSource { public: virtual ~LoopSource() = default; };
class Loop : public Executor {
public:
	// Executor::post (executor.hpp): any thread; never runs `work` inside the call; serial, in post order. This is the
	// node's own intake: accepted until step 4. Work posted after it is destroyed unrun, on the caller's thread.
	/// The loop's thread, or staged. Once, `delay` from now.
	[[nodiscard]] virtual std::unique_ptr<LoopSource> after(std::chrono::milliseconds delay,
	                                                        std::move_only_function<void()> due) = 0;
	/// The same, every `period` (CLOCK_MONOTONIC), fixed rate; ticks missed while a handler ran coalesce into one.
	[[nodiscard]] virtual std::unique_ptr<LoopSource> every(std::chrono::milliseconds period,
	                                                        std::move_only_function<void()> due) = 0;
	/// Each time `wake` was rung, in 6.3's order.
	[[nodiscard]] virtual std::unique_ptr<LoopSource> onWake(Wake& wake, std::move_only_function<void()> rung) = 0;
	/// Each time `fd` is readable. The descriptor is the caller's and outlives the source.
	[[nodiscard]] virtual std::unique_ptr<LoopSource> onReadable(int fd, std::move_only_function<void()> readable) = 0;
	virtual LoopKind kind() const noexcept = 0;
	virtual bool inLoop() const noexcept = 0;   // the calling thread is this loop's, now
protected:
	~Loop() = default;
};
class NodeLoop : public Loop {   // the framework's side
public:
	/// Main thread. Work from outside the node (slice 5's actions): posted while the loop is open to others, else
	/// destroyed unrun at once on the main thread (its reply object answers from its destructor).
	virtual void postFromOutside(std::move_only_function<void()> work) = 0;
	/// Main thread, at I5 step 1: refuses outside work from now on, and destroys queued outside work unrun on the loop's
	/// thread. The node's own work and sources go on; a handler already running finishes.
	virtual void closeToOthers() = 0;
	/// Main thread, at I5 step 4: takes no more work, runs the work already queued, destroys its state on its own thread,
	/// cancels every source still armed (counting them), then calls `closed(leftSources)` on the main thread. Never joins
	/// on the main loop.
	virtual void close(std::move_only_function<void(std::size_t leftSources)> closed) = 0;
};
class LoopAdapter {  // ear_runtime: StrandPool and DedicatedLoops behind one adapter
public:
	/// Main thread. The loop runs (its thread started, or its strand registered) when `ready` is called, on the main
	/// thread, never inside make; `failed` with the reason otherwise (start-failed).
	virtual std::unique_ptr<NodeLoop> make(const NodeName& node, std::string_view name, LoopKind kind,
	                                      ReadyEvents& events) = 0;
protected:
	~LoopAdapter() = default;
};
```

- **A loop's state** (G14, revised for R23). A loop declared with a state type (`loop::dedicated<"sendspin", Worker>`)
  is given its state in `configure` on creation (`cx.loop(worker, std::make_unique<Worker>(...))`, constructed on the
  main loop with no I/O and no threads, framework page I11). From then on the instance holds no reference to it:
  `NodeOf::post(worker, fn)` and `cx.post` run `fn(Worker&)` on the loop, and `cx.whenReady` runs a continuation
  there (framework page I14, D-A4). The state lives as long as the node's configuration: through stops, starts and
  in-place changes; it is destroyed on its own loop at teardown step 4, which is where its destructor may join the
  threads its library owns. A loop declared without a state (`loop::dedicated<"notify">`) is an executor other states
  use through `cx.executor(handle)`.
- **Staging, ended at `configured`.** Everything sent to a loop from the main loop before the node is configured
  (sources armed and work posted from `configure`, or from a state's constructor) is staged on the main loop, not handed
  to the loop's thread. At T3 the framework releases the staged items to their loops, in the order they were made. If the
  configuration is torn down before (a claim refused, a store distrusted, a block lost while configuring), the staged
  items are destroyed unrun. So no handler runs before every resource the node asked for is ready (framework page 6.4).
  Work that must wait for the code to drive the block is posted by `start()` (sendspin's `begin()`, 4.1).
- **In an in-place change** a loop and its state are the node's, unchanged. `configure` may post to it; such work is
  staged until `configure` returns, then released, so a hook never races its own posts. `whenReady`'s continuation is
  posted later (6.2).
- **The source contract.** A state destroys the sources it armed, on their loops, before or in its destructor (which
  runs on its loop). The framework counts live sources per loop. At step 4 a source still armed after the state is gone
  is a type bug: the loop cancels it itself (so its handler never runs again), the harness fails the test, debug builds
  assert, and a release build says `node <n> ear.loop-source-left: <k> sources were still armed on loop <name> when
  the node's configuration closed` and goes on. (A source armed on another loop, as sendspin's worker arms the
  notifier's `onWake`, is destroyed by a post to that loop, as `end()` does in 4.1.)
- **Two intakes.** From step 1 a loop refuses work from outside the node (`closeToOthers`): slice 5's actions use
  `postFromOutside`, so a queued call is answered `restarted` or `stopping` at step 1, and one already running finishes
  and replies before the goodbye `stop()` posted behind it. The node's own posts and sources (the goodbye, a posted
  continuation) are accepted until step 4.
- **Threads.** A handler runs on the loop's thread, one at a time per loop. A strand runs on a framework pool thread
  (`min(4, hardware threads)` to start, G9); a dedicated loop has its own thread, named `ear-<name> <node>`, cut to 15
  bytes. Nothing here is realtime. A dedicated loop may block (a library's poll, a disk write, a source write with
  patience); a strand must not: debug builds assert at every blocking primitive (`SourceWriter::write` and `idle`,
  `Store::replace`) that the caller is not a strand.
- **A slow strand.** A strand handler running past 100 ms is `node <n> ear.loop-slow: loop <name> ran one handler for
  <ms> ms`. Past 1 s the pool starts a replacement thread (at most 16 in all), so one stuck strand delays no other node
  (R16); the stuck thread rejoins the pool when its handler returns. A dedicated loop is never judged.
- **Lifetime.** Started at readiness step 3; closed to others at step 1; at step 4 it runs what was posted, destroys its
  state on its own thread and closes; freed at step 5, after the instance. A dedicated loop's thread is joined at step
  5, after it posted that it exited, so the join returns at once. A loop of a configuration that never got as far as
  step 3 is never started; its state, if given, is destroyed on the main loop.
- **What a loop is not.** No priority, no affinity, no realtime scheduling; no futures; no sharing across nodes or
  configurations. A type
  that wants a 10 ms poll asks for it (`every(10ms)`).
- **Harness.** Manual: each loop is a queue the test turns, on a fake clock (section 10).
- **Replaces.** `SendspinWorker`'s two `std::thread`s (`worker.cpp:93`, `200`), its stop `EventFd`, `waitForStop`, the
  10 ms `poll()` loop (`worker.cpp:278-299`) and the notifier's (`309-323`) [V]. `ConnectionWatch` stays the type's: a
  pure judge the tick feeds.

### 6.5 A source's writer: one at a time, and its closing contract

The source companion is the framework page's (9.6: its ring, its realtime side, keeping time, played reports). This is
the home of what a type's thread sees when it writes one.

```cpp
class SourceWriter {   // cx.source(bank, shape) returns it
public:
	/// One writing thread at a time: a dedicated loop or a thread a library owns, never a strand (asserted). Up to
	/// `frames` frames from `interleaved`; with no room waits up to `patience`. Returns frames accepted. Closed: below.
	virtual std::uint32_t write(std::span<const float> interleaved, std::uint32_t frames,
	                            std::chrono::milliseconds patience) = 0;
	/// The same thread: waits `patience`, accepting nothing. For a writer that has nothing to give but must not spin
	/// (sendspin's sync thread on a format it did not ask for). Closed: below.
	virtual void idle(std::chrono::milliseconds patience) = 0;
	/// Any ordinary thread. What was written so far is gone at the next cycle, not counted played.
	virtual void clear() noexcept = 0;
	virtual bool closed() const noexcept = 0;
	virtual std::uint32_t capacityFrames() const noexcept = 0;
	/// The framework page's 9.6: what played, and the wake rung after each cycle that played or skipped frames.
	virtual Played played() const noexcept = 0;
	virtual Wake& playedWake() noexcept = 0;
protected:
	~SourceWriter() = default;
};
```

- **One writer at a time, handed over by a join or a post.** The ring is single-producer (`FrameRing`, `frame_ring.hpp`
  [V]). A type may change which thread writes (sendspin's new client brings a new sync thread, 4.2), but only after the
  old writer's last `write` happens-before the new writer's first: the old thread joined (`client->stop()` joins the
  library's threads, `client.h:341-343` [V]), or a post from the old writer's loop to the new one's. Debug builds record
  the writing thread's id and assert when a second thread writes while the first has written in the last cycle's span
  and was not handed over (`writer.handOver()`, a no-op in release builds, marks the hand-over).
- **Closing.** The framework closes the writer at I5 step 1. A `write` or `idle` already waiting when the close comes
  wakes and returns (0 frames) at once, so a writer blocked on a long patience releases its thread for the goodbye. A
  `write` or `idle` that *begins* after the close accepts nothing and waits out its whole patience before it returns 0.
  So a library thread that calls again and again (sendspin-cpp's sync thread calls `on_audio_write` with 20 ms,
  `src/sync_task.cpp:56-58`) [V] sleeps between calls from step 1 until step 4 stops it, and never spins. The cost: a type's
  own patience bounds how long its writer can delay its goodbye after the close (sendspin's: 20 ms).
- **A replaced source** (`cx.source`, make-before-break, framework page 6.6) has its own writer; the old writer is
  closed at the change's end, by the same contract.
- **Implementation.** sendspin's `feed.cpp` (the writer's flag and fences, the room eventfd, `feed.cpp:45-69` [V])
  moves into the source companion's non-realtime half in `ear_runtime` (3b builds it; this contract is what its tests
  prove). The closed branch's wait is a `poll` on an eventfd nothing writes after the close, so it sleeps.
- **Harness.** The framework page's fake source ring, with a writer that never waits (patience ignored, returns what
  fits; `idle` returns at once), so a test on one thread cannot hang.
- **Replaces.** `Feed`'s writer side and the stop-eventfd wait in `on_audio_write` (`worker.cpp:125-131`) [V].

### 6.6 A sink's consumer: begin, and pause

The sink companion is the framework page's (9.6: stamped chunks, its ring, its thread, its drops, its arming). This is
the one home of `SinkConsumer` and `SinkControl`, declared whole here; the framework page cites them.

```cpp
class SinkConsumer {                                   // a loop state's member, or given whole: consumedBy (6.10)
public:
	/// The sink's thread, once, before any consume and before the node is `configured` (readiness step 4'). Open what
	/// the consumer writes to (file's WAV, a socket). A failure is handle.lost(detail): the node fails `start-failed`.
	virtual void begin() {}
	virtual void consume(const Chunk& chunk) = 0;      // in order; may block on I/O; framework page 9.6
	virtual void finish() = 0;                         // once, after the last consume (I5 step 3)
protected:
	~SinkConsumer() = default;
};
class SinkControl {                                    // cx.sink(...).control(): a loop's state keeps it
public:
	/// Any of the node's loops. The sink's thread finishes a consume in flight, then discards chunks (counted as paused
	/// frames, not drops) until resume(); `paused` is posted to `loop` once no consume can run. Until resume(), the
	/// consumer's state may be used by the thread that runs `paused`: the post is the hand-over. Pausing a paused sink
	/// posts `paused` at once.
	virtual void pause(Loop& loop, std::move_only_function<void()> paused) = 0;
	/// The thread that paused it. The next chunk goes to consume, with its own position, so the consumer sees the gap.
	virtual void resume() = 0;
	virtual std::uint64_t pausedFrames() const noexcept = 0;
protected:
	~SinkControl() = default;
};
```

- **Why `begin`.** `file` must write its WAV header before any frame, on a thread that may block, and an operator should
  see a recording that cannot start as a failed start, not as `running` then `failed` (today libsndfile's refusal fails
  `start`, `file.cpp:76-79` [V]). `begin` runs on the sink's thread once `configure` returned and `consumedBy` named the
  consumer, so `configured` waits for it (step 4'), and the WAV is open before the first chunk. No frame reaches the ring
  before the sink is armed, in the same invoke as the code's drive at T5, so nothing is discarded (revision 2's visible
  change 4 is gone).
- **Why `pause`.** A consumer that writes into something its node swaps in place (sendspin's library client, 4.2; the RTP
  sender's socket, 4.3) needs a moment when no `consume` runs. Without it every such type writes a lock around its
  target on the sink's thread; with it the framework owns the one hand-over, and the type's swap reads as a sequence
  (G21). Paused audio is discarded, not queued: a swap is a break in what the node sends, and the chunks' positions say
  where.
- **Stop and start.** A stop disarms the sink in the barrier's invoke and drains it to its consumer; a start re-arms it
  in the drive's invoke. The consumer is not told (no `finish`): it sees the gap in the chunks' positions, and stays
  begun while the node is stopped.
- **Teardown.** The sink is disarmed in the barrier's own invoke at step 1 (framework page I4), so the ring holds
  exactly the cycles the code drove, and step 3 drains them, none of the silent block's zeros after them. A paused sink
  at step 3 is drained by discarding, then `finish` is called; a consumer's `finish` must cope with a pause it never saw
  resumed. A replaced sink (`cx.sink`) drains to its own consumer and finishes at the
  change's commit point; its replacement's `begin` runs as it becomes ready, before the commit point.
- **Counters.** `pausedFrames` beside the framework's drops; slice 5 samples both (control-plane.md).
- **Harness.** The framework page's fake sink hands chunks straight to the consumer on the test's thread; `begin` runs at
  `h.add()`'s readiness; `pause` posts `paused` to the named loop at once; `h.paused(bank)` and `h.pausedFrames(bank)`.
- **Replaces.** `ToServer`'s plain reference to the library's source role, which "outlives every consume()" only
  because nothing swaps the client today (`worker.cpp:367`) [V]; `file`'s `sf_open_fd` on the main loop.

### 6.7 The I/O executor

**Purpose.** The framework's one thread for disk work, so a slow disk delays no node's loop and never the main loop
(framework page 7.3). Shared by every node (framework page 6.8's table), and bounded.

```cpp
class IoExecutor : public Executor {   // ear_core interface; ear_runtime: IoThread, one thread, made at boot
protected:
	~IoExecutor() = default;
};
```

- Types never see it. In slice 3c it opens stores and recordings (readiness step 2 and an in-place change's replacements) and
  runs every store write; slice 8's `/proc` reads and slice 9's runtime files will use it.
- **Jobs hold shared state.** Every job holds a `shared_ptr` to the state it fills (the store's text and mode, the
  recording's descriptor), never a `NodeStore`, `NodeRecording` or `ReadyEvents` pointer. Its completion is posted to the
  main loop and checked there against the configuration's generation and the object's liveness: a completion for a configuration that is
  gone, for an in-place change that ended, or past its deadline, is dropped, and a descriptor it opened is closed by a second
  I/O job. So a configuration torn down while its open is in flight frees nothing a job will touch.
- **One thread**: a hung disk stalls every open and every store write (section 16). Each has a 5 s deadline, so a node
  fails visibly instead of starting forever (G13).
- **Stopping.** `main.cpp` asks it to stop after `NodeHost::stopAll` completed and the keeper's thread was joined:
  queued jobs are destroyed unrun, and the job in flight gets 2 s. If it is still busy then, ear writes a `log` record
  (`source` `ear`, `warn`: `the I/O thread was still busy at exit: <job>`) and ends with `std::_Exit(0)` once the output
  queue is written out, as it does for a leaked configuration (`main.cpp:312-315`) [V]. `ear.conf` is written by then.

### 6.8 Stores

**Purpose.** A file that belongs to one node and survives its runs: named by the framework, kept narrow, read and
trusted before any of the node's loop work runs, replaced only if unchanged, swept at boot. A store is the node's configuration's for its whole life, so
an in-place change keeps it (sendspin's identity survives a port change, 4.2).

**Declaration** (in the spec, so the boot sweep knows every type's stores before any node exists):

```cpp
// ear/core/type_spec.hpp
struct StoreRule {
	std::uint32_t mode = 0600;                                  // the most it may be; wider is narrowed, never refused
	std::uint32_t maxBytes = 0;                                 // required: a larger file is not read (not trusted)
	std::string (*initial)() = nullptr;                         // the text a missing file is created with; null: none
	std::optional<std::string> (*distrust)(std::string_view) = nullptr;   // why a text is not trusted; null: any text
};
namespace store {
/// One store of a type: its suffix (as a bank's name) and its rule. Refused at compile time: maxBytes 0 (left out),
/// maxBytes above 1 MiB, a mode with bits outside 0777.
template <spec_detail::BankName Suffix> consteval StoreHandle<Suffix> file(StoreRule rule);
}
template <auto... Handles> struct Stores;                       // unique suffixes, checked at compile time
```

`NodeType` gains `std::span<const StoreDeclaration> stores() const noexcept`, answered from the spec like
`settingKinds()`, not virtual.

**The node's side:**

```cpp
class Store {
public:
	/// "<type>-<node>.<suffix>" (I9).
	virtual const std::string& fileName() const noexcept = 0;
	/// What the file held when opened and trusted (or what it was created with); then the text of the last write that
	/// committed. Any thread; changes only at the moment a write commits.
	virtual std::string text() const = 0;
	/// Blocking: a dedicated loop or a library's thread, never a strand (asserted), the main loop or the realtime thread.
	/// Queued on the I/O executor behind every earlier write of this store, then: narrows the mode, swaps `text` in only
	/// if the file still holds text(). Waits at most 5 s. Error: a sentence naming the file ("<file>: changed by
	/// something else", "<file>: larger than <n> bytes", "<file>: not written within 5 s", or the folder's), the file
	/// left as it was and text() unchanged (a write that commits after its 5 s still updates text() when it does).
	virtual std::expected<void, std::string> replace(std::string text) = 0;
	/// The same without waiting, from any of the node's loops; `done` runs on `loop` with the result, or never once the
	/// loop closed.
	virtual void replaceLater(std::string text, Loop& loop,
	                          std::move_only_function<void(std::expected<void, std::string>)> done) = 0;
protected:
	~Store() = default;
};
class StoreAdapter {   // ear_runtime: ConfinedStores, over the graph file folder (ear_folder) and the I/O executor
public:
	/// Boot, main thread, before any node: narrows every "<type>-*.<suffix>" and ".<type>-*.<suffix>.ear-staging" of
	/// every declared store to its mode, whatever its node or text. One sentence per file it could not narrow.
	virtual std::vector<SweepFinding> sweep(std::span<const std::unique_ptr<const NodeType>> types) = 0;
	/// Main thread. Opens on the I/O executor: read (no symlink, at most maxBytes), narrow, judge with `distrust`,
	/// create with `initial()` when missing; `events.ready()` or `events.failed("<file>: <why>")` on the main thread.
	virtual std::unique_ptr<NodeStore> open(const NodeName& node, std::string_view type, const StoreDeclaration& store,
	                                       ReadyEvents& events) = 0;
protected:
	~StoreAdapter() = default;
};
```

- **Writes are serialised.** Every write of a store is one I/O job, in the order made, each against the text the one
  before committed, so "changed by something else" means something outside ear.
- **Names.** `<type>-<node>.<suffix>` (I9): sendspin's is `sendspin-<node>.key`, today's name, so no file moves. The
  staging name stays `ConfinedFolder`'s (`.<name>.ear-staging`, `confined_folder.cpp:239`) [V].
- **Folder.** The graph file folder, as today. The keeper's watch ignores other names (`sendspin.md` section 8).
- **Open** at readiness step 2, after the store's claim was granted (6.9), never in `configure`, so no loop work
  reads a file. The failures are today's sendspin texts, now the framework's, so the system tests keep their lines
  (`sendspin_pairing_test.cpp:206`: `node ma failed start-failed: sendspin-ma.key: malformed: not JSON`) [V]: the
  framework writes `<file>: <why>`, and sendspin's `distrust` returns `malformed: not JSON`.
- **Mode.** Narrowed at the boot sweep, at every open (before the text is judged: a refused file may hold a private
  key), and before every write. These are today's three narrowings (`key_store.cpp:51-62`, `75-76`, `121`) [V], now one
  rule for every store.
- **Never a new identity over distrusted text.** A file `distrust` refuses is left as it was and fails the node; only a
  missing file is created.
- **Boot sweep findings.** A `log` record at `warn` with `source` the type's name, as today (G11).
- **Exclusive.** Its implicit claim, `store:<file name>` (6.9), is judged before the open. One run per node name at a time
  (I7) already makes a store one configuration's; the claim covers the leaked configuration that I7 does not.
- **Lifetime.** Opened at readiness (5 s: `<file>: not opened within 5 s`); usable until step 4; kept through every
  in-place change, stop and start; freed at step 5, after the loops (a write in flight holds the store's shared state, and its `done` is
  dropped).
- **Harness.** In memory, by handle (section 10).
- **Replaces.** `NodeKeyStore::open` (`key_store.cpp:64-97`), `narrowKeyFiles` (`51-62`), its call and boot log
  (`sendspin_type.cpp:137-144`), and `NodeFolders` entirely [V]. `NodeKeyStore` keeps only the library's provider
  interface (`load_blob`, `save_blob`) over the parsed record, writing with `Store::replace` from the worker or a library
  thread.

### 6.9 Claims

**Purpose.** Something ear-wide that one run at a time may hold (I8): a listening TCP port, a UDP port, and, implicitly,
a store's file and a recording's path. A claim is bound to a setting, so an in-place change may swap it make-before-break.

```cpp
namespace claim {
/// Each takes a checked whole number (a row's value) and throws std::logic_error outside 1 to 65535: a type bug, since
/// the row's range should have refused it.
struct TcpListen { explicit TcpListen(std::int64_t port); std::uint16_t port; };   // "cannot listen on port <p>: <why>"
struct Udp { explicit Udp(std::int64_t port); std::uint16_t port; };               // "cannot bind UDP port <p>: <why>"
}
class PortProbe {   // ear_runtime: KernelPortProbe; main thread, non-blocking socket, bind, close
public:
	/// TCP: all addresses with SO_REUSEADDR, so a TIME_WAIT passes and a live listener refuses (as today,
	/// port_claims.cpp:20-34). Error: the kernel's reason.
	virtual std::expected<void, std::string> tcpListen(std::uint16_t port) = 0;
	/// UDP: all addresses with neither SO_REUSEADDR nor SO_REUSEPORT. PipeWire's rtp-source binds with SO_REUSEADDR
	/// (pipewire-1.4.2/src/modules/module-rtp-source.c:252, bind at :309) [V], so a probe that set it would pass while
	/// a receiver holds the port.
	virtual std::expected<void, std::string> udp(std::uint16_t port) = 0;
protected:
	~PortProbe() = default;
};
```

The registry is core's (`ClaimRegistry`, in `NodeHost`, main loop only), keyed by kind and value: `tcp:8928`,
`udp:5006`, `store:sendspin-ma.key`, `recording:take1.wav`. A recording's key is its path with `.` and empty components
removed (`./a//take1.wav` is `recording:a/take1.wav`); `..` and absolute paths are already refused by `file`'s check.

| Claim | Made by |
|---|---|
| `tcp:<p>` | `cx.claim(claim::TcpListen{p})`, `cx.claim(...)` (sendspin listening) |
| `udp:<p>` | `cx.claim(claim::Udp{p})`, and implicitly every RTP receiver companion (`rtp`) |
| `store:<file>` | implicitly, every store of the spec, for every node |
| `recording:<path>` | implicitly, every `cx.recording(...)` and `cx.recording(...)` |

**Judged at readiness step 1**, or when an in-place change's hook returns, against the key's holder:

| Holder | Outcome | Detail (when it fails) |
|---|---|---|
| none | granted, then probed (ports only) | the kernel's reason |
| the same node (an in-place change asking a key its node holds) | its own: granted, not probed (G22) | — |
| a configuration that is not going: a node running, stopped, configuring or reconfiguring | refused at once | `held by node <n>` |
| a disabled node | — (it holds no claim; its settings may name the key, but nothing is claimed until it is enabled, and an enable judges its claims afresh, as here) | — |
| a configuration that is **going**: any between I5's step 1 and step 5, of any node (a restart's, a failure's, a removal's or a rename's old one) | waits: `ear.claim-wait` says why; granted when its step 5 releases it, then probed | after 15 s (in an in-place change, its deadline): `held by node <n>, which did not let go within 15 s` |
| a leaked configuration (I5a), port | abandoned: probed afresh; granted if it passes | the kernel's reason, and the node starts by itself at the late close (R15) |
| a leaked configuration, store or recording | refused | `held by node <n>'s earlier configuration, which did not close; it starts when that closes` (R15) |

- **The wait, visible.** When a wait begins, `node <b> ear.claim-wait: <key> held by node <a>, which is closing` (a
  `diagnostic` record, in `output-records.json` from S3c.5). The holder's own close deadline (10 s) passes before the
  waiter's (15 s), so a wait ends in a grant, a port's abandonment, or a refusal that names the leaked configuration.
- **A node refused by a leaked configuration starts by itself when that one closes late** (R15). The leaked configuration's late close
  (6.10) releases its claims, then starts again, once, every node still `failed` by a refusal that named it, as the
  `restart` action would (framework page T15). A port whose fresh probe failed while a leaked configuration held it is treated the
  same.
- **In an in-place change** (framework page 6.6, step 3): `cx.claim` is judged when the hook returns, before any other new
  acquisition; a refusal ends the change (T11) and destroys its continuation unrun. `cx.unclaim` names a key the node
  holds (else `std::logic_error`) and releases it at the change's end, after the replaced resources, last. So a
  node moving from 8928 to 8929 holds both for a moment, and an operator's second node can take 8928 only once the
  first is `running` on 8929. A claim wait inside an in-place change counts against the change's deadline (10 s by
  default), not its own 15 s; past it the fallback restart's `configure` waits afresh. Two nodes swapping ports in one edit
  each find the other's claim held by a current configuration, so both fall back to restarts; each restart's claim then
  finds the other's old configuration going, and waits for its step 5, whichever teardown finishes first (framework page 6.6, step 7, and
  I8). A node never waits on its own old configuration: its `configure` comes after that one's step 5 (I7).
- **A replacement on the same key.** An RTP receiver replaced with its port unchanged (`rtp` changing `format` on 5004)
  asks `udp:5004`, which its run holds: its own. Two rtp-source modules bound to one port with SO_REUSEADDR would split
  or duplicate the datagrams [U13], so the framework replaces such a companion break-first without the type asking: it
  holds the public banks the receiver feeds through the pass-through, waits for the hold's acknowledgement (the fade
  reached zero, framework page 6.6), destroys the old receiver, then makes the new one; so the gap is a fade, silence
  and a fade in, whatever the type held. A sendspin port change never asks
  the same key (4.2 claims only when the port changes).
- **Release.** At step 5, last, or at an in-place change's end for an `unclaim`. A waiter is granted in arrival order. A
  waiter of a configuration torn down, or of an in-place change ended, before its claims were granted leaves the queue at once.
- **The probe's limit stays:** a program taking the port just after the probe is not caught; sendspin's library retries
  every 5 s (`sendspin.md` section 10), and an `rtp` receiver logs PipeWire's bind failure as today.
- **mDNS names are not claimed.** sendspin's instance name is `ear-<node>-<first 8 of client id>` (AGENTS.md), and the
  client id comes from the node's own key file, so two nodes cannot advertise one name.
- **Harness.** Granted by default; `h.holdClaim(claim::TcpListen{8928}, "porch")` refuses one, `h.waitClaim(...)` makes
  one wait and `h.releaseClaim(...)` ends the wait, `h.failProbe(...)` makes a probe fail; `h.claims()` lists what the
  run holds, both ends of an in-place change included.
- **Replaces.** `PortClaims`, `HeldPort`, `probeListenPort` (`port_claims.*`, `sendspin_type.cpp:36-47`, `83-92`) [V],
  and their type-level scope: a claim now spans types (an `airplay` node on 7000 against anything else on 7000; an `rtp`
  receiver against an `rtpsend` sender on one port).

### 6.10 Teardown, full and partial

I5 (framework page 6.9) with every primitive of this page. Every step that waits on another thread is asynchronous; the
main loop never joins a thread that has not already said it is done.

| Step | What | Threads it waits on |
|---|---|---|
| 1 | `stop()`, if the node was running; the fade and its acknowledgement, then the barrier (`drive(nullptr)`, or the block destroyed), which disarms every sink in the same invoke; `release`, holds cleared; every source writer closed (6.5); every loop closed to others (queued actions answered); an in-place change in flight ended (its pending acquisitions dropped, its continuation destroyed if not yet posted, its token ignored); if the node is not configured yet: its claim waits leave the queue, its pending opens are orphaned (6.7), and the readiness steps it had not reached never start | the data loop, asynchronously: the step continues from the invoke's completion, posted back (framework page I4) |
| 2 | Companions destroyed (the lifecycle's) | none |
| 3 | Sinks drained: each hands what its ring holds to its consumer (a paused sink discards it) and calls `finish` on its own thread, then reports | sinks' threads |
| 4 | Each loop closes: it takes no new work, runs the work already posted (`stop()`'s goodbye, a posted continuation), then destroys its state on its own thread (the state's destructor destroys the sources it armed and stops and joins the threads its library owns), cancels and counts any source still armed (`ear.loop-source-left`), and reports | loop threads, and the threads a state joins |
| 5 | On the main loop: the code; the instance; the companions' rings and wakes; loops (a dedicated thread joined, already exited); stores; recordings closed; claims released (waiters granted); the config | none |

**A configuration torn down before it was ready** (a change, a removal or a failure while it waits on a claim, an open
or a companion): step 1 as above, without `stop()`; steps 2 and 3 for what exists; step 4 closes whatever loops
readiness step 3 had started (their staged items destroyed unrun, their states destroyed on their threads); loop states
whose loop never started are destroyed on the main loop with the staged items, which never ran; step 5 frees what
exists.

**A stop** (framework page T6) is step 1's first half only: `stop()`, the fade, the barrier with every sink disarmed,
and each sink drained to its consumer; the configuration, its loops and their states, its claims, stores, recordings
and companions stay (R26).

**A disable** (framework page T20) is the whole table, as a restart's teardown is, except that nothing is configured
after step 5: `stop()` and the fade only if the node was running (a stopped node is already silent), then steps 1 to 5;
the block stays. Its claims are released at step 5, so a node waiting on one (`ear.claim-wait`) is granted then; a
disable that passes the close deadline leaves a leaked configuration as any teardown does (I5a), and the node is
`disabled` all the same.

**An in-place change's commit point** (framework page T10) releases what it replaced by the same steps, for those resources only, after
its commit point's one invoke (the new code and layout installed, replaced sinks disarmed and their replacements
armed): a replaced source's writer closed (1); replaced companions destroyed (2); replaced sinks
drained to their own consumers and finished (3); no step 4 (the loops and their states go on); replaced recordings
closed, then `unclaim`ed claims released, last (5). **An in-place change that is superseded** (framework page T18) releases
its uncommitted replacements by the same partial steps, and the node keeps everything it had. **One that
falls back** (T11) tears the whole configuration down, old and new resources alike, by the full table.

**The close deadline and the leaked configuration** (I5a; R15). The deadline (default 10 s) covers steps 3 and 4 together,
as slice 1 built for 3 and 4. A configuration past it is leaked: everything step 5 would free is kept, an `ear.close-overdue`
diagnostic names what was still open (`<n> sinks open, <k> loops open`), its port claims are
abandoned and its store and recording claims stay (6.9), and the node moves on. Unlike slice 1 (`node_host.cpp:446-464`,
where the detached `Channel` drops every later report, `191-221`) [V], the leaked configuration keeps listening:

- Its handle is re-pointed at a **leaked-configuration record** that `NodeHost` keeps (in a list, never freed). Every later sink
  report and loop close lands there, in order on the main loop.
- The record carries on where the teardown stopped: a configuration leaked in step 3 has its loops closed (step 4) once
  its last sink reports; one leaked in step 4 waits for its loops' reports.
- When everything it held has reported, its claims are released (waiters granted), `node <n> ear.close-late: the configuration
  leaked at the close deadline closed after <s> s; its claims are released` is said, and every node its claims refused
  is started again (6.9, R15). Its memory stays leaked: the rule "a leaked configuration is never freed" stays simple.
- `NodeHost::leakedConfigurations()` counts leaked configurations still open; one that closed late no longer counts, so a stop after it ends
  normally rather than with `std::_Exit` (`main.cpp:312-315`) [V].
- A run whose threads never report stays leaked and open until ear exits, and the nodes it refused stay `failed` with a
  detail that says why.

### 6.11 Recordings, deferred

**Purpose.** A file beneath the recordings folder that one run writes (slice 2's kind, `node_type.hpp:164-167` [V]), now
opened after its claim and off the main loop, written by a sink's consumer.

```cpp
struct RecordingRequest { std::string path; };   // unchanged: relative to the recordings folder
// configure:          int fd = cx.recording({path});  claims recording:<normalised path>; the descriptor, open,
//                                                      created or truncated, usable once configured
// configure, in place: cx.recording({newPath});        claimed and opened beside the old; the old closed at the
//                                                      commit point
class RecordingAdapter {   // node_host.hpp, amended; main.cpp's ConfinedRecordings over ear_folder
public:
	/// Main thread. Creates or truncates `path` beneath the folder on the I/O executor (openat2, RESOLVE_BENEATH, as
	/// today); `events.ready()` with the descriptor in the node's shared state, or `events.failed(<why>)`.
	virtual std::unique_ptr<NodeRecording> open(const NodeName& node, const std::string& path, ReadyEvents& events) = 0;
protected:
	~RecordingAdapter() = default;
};
```

- **Order.** Asked for in `configure`, judged as a claim at step 1, opened at step 2, the WAV header written in the
  sink consumer's `begin` (6.6), before the node is configured. So a path held by a running node is refused before
  anything is truncated, and a path held by a node still closing waits (I8): removing `rec` and adding `rec2` on
  `take1.wav` in one plan works.
- **Failure text.** The framework writes `cannot open <path>: <why>`, today's text (`file.cpp:71`, `79`) [V], so the system
  tests keep their lines; a claim refusal is `cannot open <path>: held by node <n>`; libsndfile's refusal in `begin`
  is `cannot open <path>: <libsndfile's reason>`, before the node is configured, as today.
- **`file` on it.** `configure` asks for the recording and a sink on its inside bank `writer` (framework page 9.6), and
  gives the sink its `WavWriter` whole, around the descriptor (`consumedBy`); `begin` calls `sf_open_fd`; `consume`
  writes; `finish` closes the WAV. `path` stays restart-only (framework page 6.6); `cx.recording` exists for a later
  gapless switch.
- **Lifetime.** Opened at readiness (5 s); closed at step 5, after stores and before claims are released, so the next
  configuration's open of that path follows the close; leaked with a leaked configuration, its claim kept (6.9).
- **Harness.** A real descriptor on a temporary file, or a failure the test sets (section 10).
- **Replaces.** The synchronous open in `acquireResource` (`node_host.cpp:126-141`) and `file`'s main-loop
  `sf_open_fd` (`file.cpp:76`) [V].

### 6.12 What a type author no longer meets

For the reader of revisions 2 and 3: `StartOf<spec>`, `SetupOf<spec>`, `ReadyOf<spec>`, `BuildOf<spec>`, `Built`,
`ReconfigureOf` and the recipe are gone; one `ConfigureOf<spec>` is handed to `configure` on creation and in place, the
instance keeps a `NodeOf<spec>`, and a loop's state, not a run object, owns what a type's threads use (G15, R19, R23).
A type's `configure`, `start` or `stop` of the wrong signature, or a post typed for another loop's state, does not
compile (the lifecycle's compile-fail cases); hand-made `NodeTypeOf` types are test-only after S3c.6.

### 6.13 `TypeSpec`, whole: the one home of its members

Both pages' members, in their final order. Every template parameter after `BankList` defaults to an empty list, so a
spec gives only what it uses, in this order (designated initialisers follow declaration order).

```cpp
// ear/core/type_spec.hpp
template <class Settings, class BankList, class StoreList = Stores<>, class LoopList = Loops<>,
          class Status = StatusTable<>, class Events = EventTable<>, class Actions = ActionTable<>,
          class Logs = LogCodes<>>
struct TypeSpec {
	Settings settings;      // slice 2; rows' .inPlace() and .live are slice 3d's and 6's marks on them
	BankList banks;         // slice 2; inside banks name their companions (framework page 9.6)
	StoreList stores{};     // slice 3c (6.8)
	LoopList loops{};       // slice 3c (6.4)
	Lifecycle lifecycle{};  // slice 3d: the in-place deadline, 1 s to 60 s (framework page 6.10)
	Status status{};        // slice 5 (control-plane.md)
	Events events{};        // slice 5
	Actions actions{};      // slice 5
	Logs logCodes{};        // slice 5
};
```

Slice 3c builds the first four; slice 3d adds `lifecycle` (not a template parameter: one struct for every type,
`Lifecycle{.reconfigureDeadline = 30s}`, refused at compile time below 1 s or above 60 s; framework page 6.10 declares
it); slice 5 appends the last four in this order. `Stores`, `Loops` and `Banks` check their names' uniqueness at compile time; a store, a loop and a
bank may share a name. Whether GCC 14 deduces the defaulted parameters for every subset spelling is U1.

## 7. Threads, as one contract

The framework page's 7.4 is the contract; this table adds this page's primitives to it.

| What the type writes | Runs on | May |
|---|---|---|
| Rows' parse functions, `check`, `ports`, a store's `initial` and `distrust` | any thread, pure | nothing else (`initial` may read the clock and a random source) |
| The instance's constructor, `configure` (and the loop states' constructors it calls) | the main loop | acquire: claims, recordings, companions; take store handles, writers, sink controls, executors and the node handle; give loops their states; name consumers; post (staged until configured, or until an in-place `configure` returns); `whenReady`. No I/O, no threads, no blocking |
| `start()`, `stop()` | the main loop | post to the node's loops. No blocking |
| `prepare`, `release` | the main loop, never with `process` | allocate |
| `process` | the realtime thread | `io` only (framework page I12) |
| Strand handlers | a pool thread, serially per strand | anything that does not block: `post`, `replaceLater`, `NodeHandle`, non-blocking sockets, `SinkControl` |
| Dedicated loop handlers | the loop's thread | also block: `SourceWriter::write` and `idle`, `Store::replace`, a library's `start`, `stop` and poll |
| Threads a library owns (sendspin-cpp's) | their own | what the library does, `SourceWriter::write` and `idle`, `Store::replace`; the loop's state stops and joins them when it is destroyed, on its loop |
| `SinkConsumer::begin`, `consume`, `finish` | the sink's thread | block on I/O; `begin` opens what the consumer writes to |
| A `whenReady` continuation, a `SinkControl::pause` callback | the loop named | as that loop's handlers; while paused, the consumer's state |

A guard joins `standard_output_guard.cmake` as its own script, `thread_guard.cmake`: no non-test file under
`src/nodes/` (it skips every path with a `/tests/` component, since sendspin's tests use `std::thread`,
`code_test.cpp:272`, `feed_test.cpp:80` [V]) names `std::thread`, `std::jthread`, `std::async`, `pthread_create` or
`fork`. Its own test covers each spelling, a look-alike it lets through (`my_thread_count`), and the `/tests/` exclusion
(G12). A type gets threads only as loops, and a library's own threads stay inside the library.

## 8. Invariants and failure modes

Invariants this page adds to the framework page's I1 to I15, each a test's to prove:

- **P1. No type code starts a thread, and no type code does I/O on the main loop.** Loops are the framework's; stores
  and recordings open on the I/O executor before any of the node's loop work runs; a consumer opens what it writes in `begin`, on the
  sink's thread.
- **P2. No handler of a node's loops runs before the node is configured or after its loop's state is destroyed**, and
  every loop of a configuration is closed before its instance is freed (staging, the source contract, step 4).
- **P3. A claim has one holder ear-wide**, across types, judged before anything of the configuration is opened or started; a node
  in an in-place change may hold an old and a new key, never two nodes one key; a leaked configuration's claim on
  something no probe can check is never handed on before its late close.
- **P4. A store's file is never wider than its mode** after boot, an open or a write, and never replaced by a new
  identity when its text is distrusted.
- **P5. What a configuration keeps, an in-place change, a stop and a start keep.** Loops and their states, stores, the
  node handle and claims a change does not `unclaim` are the same objects before and after; an in-place change's new acquisitions are ready before its continuation
  runs, and its replaced ones are released only after its end.
- **P6. One writer of a source and one consumer call of a sink at a time.** A source's writer changes only by a join or
  a post; a sink's consumer state changes hands only through `pause`.

| Failure | What the operator sees | Why it is safe |
|---|---|---|
| A store cannot open: a symlink, a directory, more than `maxBytes`, unreadable, distrusted, cannot be created | `node <n> starting`, then `failed start-failed: <file>: <why>`; the file unchanged | no loop work ran, no loop started |
| A store or recording open hangs (a hung disk) | after 5 s, `failed start-failed: <file>: not opened within 5 s` (a recording: `cannot open <path>: not opened within 5 s`) | the job holds only shared state; its late completion is dropped (6.7) |
| A later store write is refused | sendspin: `says key-write-failed <detail>` (today's note, its text from the store) | the file keeps the other writer's text; the worker's state stays in memory |
| A claim held by another running node | `failed start-failed: cannot listen on port <p>: held by node <n>` (today's text, `sendspin_connections_test.cpp:235`) [V]; `cannot open take1.wav: held by node <n>` for a path | refused at step 1: no file opened, no thread started |
| A claim held by a node still closing | `starting`, `ear.claim-wait: <key> held by node <n>, which is closing`, then `running`; after 15 s, failed with `which did not let go within 15 s` | I8; the holder's close deadline (10 s) passes first |
| A port another program holds | `failed start-failed: cannot listen on port <p>: Address already in use` (today's text, `sendspin_connections_test.cpp:202`) [V]; for `rtp`, `cannot bind UDP port <p>: Address already in use` | the probe |
| A claim held by a leaked configuration | port: probed afresh; store or path: `held by node <n>'s earlier configuration, which did not close; it starts when that closes`; later `ear.close-late` and the node running by itself | R15 |
| An in-place change's claim refused | `reconfiguring: port`, `stopping: restarting (claim refused)`, then `failed start-failed: cannot listen on port <p>: held by node <n>` | refused before the continuation runs: the old client was never stopped by it |
| An in-place change's claim waits past the deadline | `reconfiguring`, `ear.claim-wait`, `stopping: restarting (the change took more than 10 s)`, the restart's own wait | the change's deadline (framework page 6.6) |
| A strand handler that never returns | `ear.loop-slow` after 100 ms; a replacement pool thread after 1 s; at teardown the close deadline leaks the configuration | other nodes' strands keep their threads (R16) |
| A dedicated loop that never drains | at teardown, after 10 s, `ear.close-overdue ... 1 loops open`; the node moves on | its memory and thread are leaked, never freed |
| A loop's thread cannot be started | `failed start-failed: cannot start loop <name>: <errno text>` | no loop work ran |
| A loop state that leaves a source armed | `ear.loop-source-left` (release build); the harness and debug builds fail | the loop cancels it at step 4 |
| A source writer blocked at teardown | returns 0 at step 1; later calls sleep their patience | its wait is bounded by its own patience |
| A consumer's `begin` fails (libsndfile refuses the descriptor) | `starting`, then `failed start-failed: cannot open take1.wav: <libsndfile's reason>` | `lost` from the sink's thread, crossing to the main loop; the node never ran |
| A consumer's `begin` never returns | after 5 s, `failed start-failed: writer: consumer did not begin within 5 s`; at teardown the close deadline covers the sink's thread | readiness step 4' has its deadline |
| A post, ring or handle call after the configuration closed | dropped | loops destroy late work unrun; the handle is detached |
| The close deadline at stop | ear proceeds, as today; `main.cpp` ends with `std::_Exit` once any leaked configuration is still open | unchanged, but a configuration that closed late no longer forces it |
| The I/O thread busy at exit | `the I/O thread was still busy at exit: <job>`, then `std::_Exit(0)` | `ear.conf` was written first |

**Visible changes**, each with a system test in its step:

1. **A corrupt key file** fails sendspin before it was `running` (today the worker reads the file after the recipe
   returned, `worker.cpp:194`, `node_host.cpp:307` [V]). The system test checks only the `failed` line
   (`sendspin_pairing_test.cpp:206`) [V]; any check of `running` before it is found in S3c.6 (U4).
2. **Two `file` nodes on one path**: today both run and corrupt one file. Now the second fails `start-failed: cannot
   open take1.wav: held by node <first>`; removing one and adding the other in one plan works.
3. **An `rtp` node on a UDP port another program holds** fails `start-failed: cannot bind UDP port 5004: Address
   already in use`, where today it stays `starting` for good (AGENTS.md). Two `rtp` nodes on one port are refused
   `held by node <n>`; two multicast groups on one port too (section 16).
4. **New diagnostics**: `ear.claim-wait`, `ear.close-late`, `ear.loop-slow`, `ear.loop-source-left`; `ear.close-overdue`
   gains its loop count. Each is in `output-records.json` from the step that adds it.
5. **sendspin changes `name`, `port` and `server` in place** (S3d.2): `node <n> reconfiguring: <keys>`, the client's
   notes (`disconnected`, `listening ...` or `dialling ...`, the same `id`), then `node <n> running`; the key file is not
   re-read, its loops and companions stay, and Music Assistant sees the same client id under its new name or port.

## 9. The carried sendspin-cpp log patch

**Today.** On a host build `SS_LOG*` is a bare `fprintf(stderr, ...)` behind one process-wide level
(`src/platform/logging.h:56-63` in the pinned tree) [V]. ear sets `WARN` (`worker.cpp:218`) [V]; its lines reach
standard output as unattributed `stderr` records at `warn`, and with two sendspin nodes nobody can tell whose. There are
533 `SS_LOG*` call sites in 36 files [V: grep of the pinned tree]; none takes a client.

**The patch** (`cmake/sendspin-log.patch`, a second carried patch beside `sendspin-listener.patch`):

1. `logging.h`, host branch: the five macros call `::sendspin::log_emit(level, tag, fmt, ...)`
   (`__attribute__((format(printf, 3, 4)))`), defined in a new `src/platform/logging.cpp`. It checks the level as now,
   formats into a 512-byte stack buffer (`vsnprintf`; a longer line is cut and marked), and calls the sink if one is
   set, else writes `stderr` exactly as today. No allocation.
2. The sink is process-wide, the context per thread:
   ```cpp
   namespace sendspin {
   using LogSink = void (*)(void* context, int level, const char* tag, const char* text, bool cut) noexcept;
   void set_log_sink(LogSink sink) noexcept;                 // atomic; null restores stderr
   class LogScope {                                          // sets this thread's context, restores the last on exit
   public: explicit LogScope(void* context) noexcept; ~LogScope();
   };
   }
   ```
3. `SendspinClientConfig::log_context` (`void*`, default null). The client opens a `LogScope` with it in every public
   entry point (`start`, `loop`, `stop`, `connect_to`, `disconnect`, the destructor, `PlayerRole::notify_audio_played`,
   `SourceRole::write_audio`), so callbacks it makes from there, the persistence provider's included, inherit it.
4. The three library threads inherit the context of the thread that starts them: `ProtocolTask::start`,
   `SyncTask::start` and `SourceTask::start` (`protocol_task.cpp:60`, `sync_task.cpp:134`, `source_task.cpp:161`) [V]
   capture the caller's context and open a `LogScope` at the thread's entry. They are started inside `client.start()`,
   which is scoped.
5. The three IXWebSocket callbacks the library registers (`client_connection.cpp:142`, `ws_server.cpp:75`, `107`) [V]
   capture the context and open a scope in the lambda. IXWebSocket's own `fprintf`s stay unattributed `stderr` records;
   `ixwebsocket-limits.patch` already bounds them per connection.

**ear's side**, in sendspin's worker state, on the worker: `set_log_sink(&forward)` once per process (`std::call_once`),
and `config.log_context = &handle_` (the node's `NodeHandle`). `forward` maps `SS_LOG_ERROR`, `WARN`, `INFO` and `DEBUG`/
`VERBOSE` to ear's four levels and calls `handle.log({.level, .source = "sendspin-cpp", .text, .topic = tag, .cut})`.
A null context becomes an unattributed `log` record from `sendspin-cpp`. Records then read:

```json
{"v":1,"seq":91,"t":"…","kind":"log","node":"kitchen","run":4,"source":"sendspin-cpp","topic":"sendspin.conn","level":"warn","text":"…","msg":"node kitchen sendspin-cpp sendspin.conn warn: …"}
```

- **Across an in-place change.** The client 4.2 makes anew gets the same context, the node handle, which an in-place change keeps,
  so its lines carry the same node as before the swap.
- **Lifetime.** The context is the node handle, which outlives the configuration (5). Each client is destroyed on the worker before
  the worker's state is (or before the swap's new one starts), so no thread of an old client logs after it is gone.
- **Cost.** A rebase at each bump of the pin, as for the listener patch; configure refuses a tree without it
  (`carried_patch_missing`, `cmake/CarriedPatch.cmake`) [V]. `sendspin_patched_dep` takes a list of patches.
- **Not upstreamed here.** Offering it upstream is worth a try (U8); ear keeps it carried until a release has it.

## 10. The type harness, for slice 3c

`TypeHarness<spec, Type>` applies operations and steps the lifecycle (framework page 7.6: `parse`, `add`, `ready`,
`blockReady`, `process`, `change`, `stop`, `start`, `restart`, `remove`) and fakes companions; this page adds the fakes of its primitives, each named by the
spec's handle (a misnamed or undeclared handle does not compile, as `harness.input(card::play, 0)` does not today,
`type_harness.hpp:23-30` [V]). `NodeHarness` under it takes names, for hand-made types.

| Kind | The fake | What a test does |
|---|---|---|
| Loop | a queue and a fake clock per loop; kind kept, so blocking asserts still judge strands | `runLoops()`; `advance(ms)`; `loop(worker)` to turn one; `armed(worker)` counts its live sources |
| Store | in memory, by handle | `presetStore(keyFile, text)`, `failStoreOpen(keyFile, why)`, `failNextWrite(keyFile, why)`, `storeText(keyFile)` |
| Recording | a real descriptor on a temporary file, or a failure | `failRecording(path, why)`, `recordingText(path)` |
| Claim | granted | `holdClaim(claim::TcpListen{8928}, "porch")`, `waitClaim(...)`, `releaseClaim(...)`, `failProbe(...)`; `claims()` |
| Readable descriptor | delivered by the test | `readable(fd)`: the `onReadable` handler runs on its loop's next turn |
| Node handle | captured | `notes()` (paced as the framework paces), `logs()`, the fact |
| Sink control | on the fake sink (framework page 7.6) | `paused(bank)`, `pausedFrames(bank)`; `begin` runs in `add()`'s readiness |
| Source writer | on the fake source; never waits | `writerThreads(bank)` in debug builds, for P6 |

**Rules a test can rely on:**

- **Readiness.** `add()` makes the instance, calls `configure`, judges claims and opens stores and recordings at once,
  then starts the loops with their states and calls every consumer's `begin`, unless the test made a claim wait or an
  open fail first; after `releaseClaim`, readiness carries on at once. Once configured, the staged items are released
  (they run on the next `runLoops()`). `blockReady()` drives the code and calls `start()`.
- **The clock.** `runLoops()` runs posted work, rung wakes and readable deliveries on every loop until all are idle; it
  never fires a timer. `advance(d)` moves the fake clock by `d` and fires, in time order, every timer due at or before
  the new now, `after(0)` included, running each loop until idle after each.
- **In place.** `change(settings)` calls `configure` with `old`; claims it asked for are judged as at `add()`; a `whenReady`
  continuation runs on the next `runLoops()`; `state()` stays `reconfiguring` until its token's `done` and every
  replacement is ready, then the replaced claims and recordings are released and `state()` is `running` (or `stopped`).
- **Store writes.** A `replace` completes inside the call; a `replaceLater` completes, and its `done` runs, on the next
  `runLoops()`.
- **`stop()`** calls `stop()`, silences and disarms; the configuration stays. **`disable()`** runs I5 whole, as
  `remove()` does, and leaves the harness's block with no configuration (`state()` is `disabled`); **`enable()`**
  configures afresh, as `add()` does. **`remove()`** (and a restart) runs I5 as
  the framework does: `stop()` if running, release, writers closed, loops closed to others, sinks finished, then the
  harness turns each loop until its posted work ran and its state is destroyed. **If a loop's state, once destroyed,
  leaves sources armed, the test fails** with `std::runtime_error` naming the loop and its sources. Then step 5. A real-time bound (10 s) applies only when the test chose to start real
  library threads.
- So a loop state that starts its library from a timer never starts it in a test that does not advance the clock, and
  sendspin's `begin()` runs only on `runLoops()` after `blockReady()`: the all-type tests run sendspin without a network.

`notYetInHarness` and its checks in `test/nodes/harness_test.cpp` go in S3c.6 (framework page 13.1).

## 11. Where the code lives

| Package | Gains | Never |
|---|---|---|
| `ear_core` | `node_handle.hpp`, `loop.hpp`, the wake, store, recording, claim and probe interfaces, `IoExecutor`, `SinkControl`, `ConfigureDone`; `ClaimRegistry` and the leaked-configuration record in `NodeHost`; the contexts' members of 6.1, `whenReady`, `StoreRule`, `Stores`, `store::file`, `Loops`, `loop::strand`, `loop::dedicated` and `TypeSpec`'s new members in `type_spec.hpp`; `NodeType::stores()` | threads, files, sockets (unchanged) |
| `ear_runtime` (`src/runtime`, from 3b) | `StrandPool` and `DedicatedLoops` (one `LoopAdapter`), `EventWake` (if 3b has not), `IoThread`, `KernelPortProbe`, `ConfinedStores` (store adapter and boot sweep, over `ear_folder`); the sink's `pause` on its thread | PipeWire; policy (what a claim means, when a store is trusted: core's and the type's) |
| `ear_type_harness` | the fakes of section 10 | PipeWire, real threads |
| `ear_node_sendspin` | loses its primitives (section 12) | `std::thread` (the guard) |
| `ear_node_file` | its WAV opened in `begin` on the sink's thread | I/O on the main loop |
| `ear_all_node_types` | `allNodeTypes()` takes no argument; the template's `made` goes | — |
| `ear` (`main.cpp`) | makes the `ear_runtime` adapters and the I/O executor, sweeps at boot (findings to `StatusLog` as `log` records), passes them to `ResourceRegistry`, `nodes.logTo(log)`, stops the I/O executor last (6.7). `ConfinedRecordings` opens on the I/O executor | computing anything |

Dependencies: `ear_core` <- `ear_runtime` -> `ear_folder`; `ear_runtime` <- `ear`. No node type links `ear_runtime`
or `ear_pipewire`: configure's `ear_refuse_node_type_links` refuses a type linking another type today, and S3c.2 asks
it to refuse these two packages too, so a type reaches them only through core's interfaces.

`ResourceRegistry` takes `ResourceAdapters{...}`, one member per kind (the lifecycle's companions and this page's
`.recordings`, `.loops`, `.stores`, `.probe`, `.io`); a null adapter refuses its kind with "no <kind> in this host" at
acquire, as a missing recordings folder does today (`node_host.cpp:34-41`) [V]. Tests give what they use.

## 12. sendspin and file, file by file

sendspin's namespace becomes `ear::nodes::sendspin` (AGENTS.md, adding a type, step 3). The table is after 3a and 3b:
`feed.*`, `mailbox.hpp`, `sendspin_code.*` and the capture anchor are already gone (framework page 13.2).

| File (lines today) | Fate in 3c and 3d |
|---|---|
| `include/ear/nodes/sendspin.hpp` | `makeSendspinNodeType()`, no argument; includes no `node_folders.hpp` |
| `sendspin_type.cpp` (146) | renamed `sendspin.cpp`: the class `Sendspin` (`configure`, its in-place part, `start`, `stop`), `makeSendspinNodeType` (about 50 lines). `SendspinConfig`, `SendspinType`, `HeldPort` and the boot sweep go |
| new `sendspin_spec.hpp` | rows (moved from `settings.hpp`, marked `.inPlace()` in 3d), banks, `keyFile`, `worker` (with its state type), `notifier`, `spec`, `Sendspin`, `feedFramesFor`, `emptyKeyRecord`, `whyNotKeyRecord` |
| `settings.hpp`, `settings.cpp` (44, 67) | `ServerUrl`, `parseName`, `SendspinSettings`, `settingsFrom`; namespace only |
| `worker.hpp`, `worker.cpp` (37, 398) | stay `worker.hpp`, `worker.cpp`: `Worker`, the worker loop's state, built from `WorkerParts` in `configure`, with `begin`, `end`, `redial` and `swapClient`, with the library's listeners (unchanged bodies but `on_audio_write`'s idle branch), `start`, `startClient`, `tick`, `notify`, `stopClient`, `close`, and in 3d `redial` and `swapClient`. Gone: both `std::thread`s, the stop `EventFd`, `waitForStop`, `poll()`, `finish`'s joins, `NoteGate`, `clientMutex_` (the sink's pause replaces it for `write_audio`; `clientTimeOf`, read only by `clock_test.cpp:434` [V], posts to the worker and waits for the answer). About 320 lines |
| `key_store.hpp`, `.cpp` (41, 141) | `NodeKeyStore` keeps `load_blob`, `save_blob` over a parsed `KeyRecord` and a `Store&`; `whyMalformed` becomes `whyNotKeyRecord`; `KeyRecord::parseTrusted`; `emptyKeyRecord`. `open` and `narrowKeyFiles` go. About 70 lines |
| `event_fd.hpp` (33) | deleted: `EventWake` and the source's room signal own their eventfds |
| `note_gate.hpp`, `.cpp` (27, 40) | deleted: the node handle paces `say` |
| `port_claims.hpp`, `.cpp` (24, 36) | deleted: `ClaimRegistry` (core) and `KernelPortProbe` (`ear_runtime`) |
| `to_server.*` | `ToServer : SinkConsumer` over stamped chunks (3b), a member of the worker's state, holding the sink's `SinkControl&` (3d) |
| `connection_watch.*`, `advertiser.*`, `pcm.hpp`, `peer_text.hpp`, `notes.hpp` | namespace only. `notes::notesDropped` goes (the framework says it). The advertiser's goodbye is called by `swapClient` too |
| `CMakeLists.txt` | the sources above; tests link the harness; `clock_test` links `ear_runtime` |

`file` (after 3b, it has a sink on `writer` and no realtime code): its `configure` adds `cx.recording({path})` and gives
the sink a `WavWriter` around the descriptor and `node.handle()` (`consumedBy`); `WavWriter::begin` calls
`sf_open_fd` and reports a refusal with `handle.lost("cannot open <path>: <libsndfile's reason>")`. Its tests keep their
cases, now through the harness's recording fake, with the header written in `h.add()`'s readiness.

sendspin's tests:

| Test file | Fate |
|---|---|
| `port_claims_test.cpp` (3) | the claim case moves to core (`ClaimRegistry`); the two probe cases (a live listener, `TIME_WAIT`) to `ear_runtime`, plus a UDP port held with SO_REUSEADDR refused |
| `notes_test.cpp` (6) | `NoteGate`'s cases move to core's node handle tests; `printable` and the texts stay |
| `keys_test.cpp` (11) | open, narrow, create, symlink, size and replace-if-unchanged cases move to `ear_runtime` (`ConfinedStores`) as store cases over a declared rule; the record's own cases (`whyNotKeyRecord`, `load_blob`, `save_blob`, `key-write-failed`) stay, over a harness store; "making the node type narrows every key file" becomes `ConfinedStores::sweep` over sendspin's declaration |
| `settings_test.cpp` (6) | the type case runs through `TypeHarness<spec, Sendspin>`; parse cases unchanged; in 3d, each row's mark |
| `clock_test.cpp` (4) | builds the node's parts with `ear_runtime`'s real adapters beside its real engine (as it builds `FilterPortBlocks` today, `clock_test.cpp:270-282`) [V] |
| new `type_test.cpp` | through the harness: listening asks `claim::TcpListen{8928}`, dialling asks none; a held claim fails with today's text; a distrusted store fails with today's text and is not written; the worker's first timer starts the client only once `start` ran and the clock advanced; close runs the goodbye on the worker after the notifier let go, with no source left. In 3d: a port change claims the new port, unclaims the old at the end, pauses and resumes the sink, and keeps the store's text and the loops; a server URL change posts a redial and is `applied`; a refused new port restarts; a stop during the swap ends with no source left |
| `test/nodes/all_types.hpp` | `notYetInHarness` deleted; `allTypes()` calls `allNodeTypes()`; the config folder and `BootLog` go |
| `test/system/*` | unchanged lines; S3c.6 checks every sendspin system test still passes on the new order of states (8); S3d.2 adds the in-place changes |

## 13. Interim seams, cleared

| Seam (section 2) | Cleared by |
|---|---|
| sendspin off `TypeSpec` / `TypeHarness` | section 12; `NodeTypeOf` then has no production user and stays for tests' hand-made types |
| `NodeFolders`, `NodeFolders::log`, the folder argument | stores (6.8): `node_folders.hpp` deleted; the boot sweep is `StoreAdapter::sweep`, called by `main.cpp` |
| `InsidesEvents` and `Channel` behind `BuildContext::events()` | the node handle (5) |
| Claims in sendspin's type; `rtp` and recordings unclaimed | claims (6.9), ear-wide |
| A leaked configuration holds its port or file | claims and the leaked-configuration record (6.10): abandoned ports re-probed; stores and recording paths refused until a late close, which starts the refused node (R15) |
| `file` does I/O on the main loop | recordings open on the I/O executor (6.11); the WAV header in `begin` on the sink's thread (6.6) |
| `notYetInHarness = {"sendspin"}` | section 10: deleted |
| sendspin's own threads, eventfds, note pacing, key file handling | loops (6.4), the framework's wakes (6.3), the node handle (5), stores (6.8) |
| The library's unattributed lines | the log patch (section 9) |
| Every sendspin settings change restarts it | its in-place `configure` (4.2) on 3d's path |

## 14. The boundary with slice 5

Slice 3c fixes the seams; slice 5 (control-plane.md revision 4) adds behaviour on them. Each fact has one home: the
first column's facts are this page's, the second's are control-plane.md's. The last column records what the two pages'
reconciliation in review round 3 settled.

| Slice 3c delivers (this page) | Slice 5 builds on it (control-plane.md) | Reconciled (round 3) |
|---|---|---|
| `NodeHandle`: `node`, `lost`, `log`, interim `say` paced per node (kept across in-place changes, stops and starts), reached by `node.handle()` on the instance's `NodeOf` (G17) | `Report<spec>` over the handle (status writes, events), reached from `NodeOf`; removes `say` once sendspin's notes are split | done: `node.handle()`, `node.report()` (control-plane.md 5.3) |
| `ConfigureOf<spec>` and `NodeOf<spec>`'s members of 6.1; the three hooks | the realtime ring the framework makes with each configuration, never acquired; the code writes it through `io.set`, `io.raise`, `io.log` | done: `gain`'s `configure` gives `cx.use(GainCode{})` (framework page 6.10) |
| Loops declared in the spec; `NodeLoop::postFromOutside` and `closeToOthers` at step 1; own work (an in-place change's continuation among it) accepted until step 4; loops and their states alive through in-place changes, stops and starts | action calls posted with `postFromOutside`; a queued call answered `restarted` or `stopping` at step 1; status fields bound to a loop by `writer::loop`; during an in-place change and while stopped, actions keep running on the node's loops | `restart` ends an in-place change in flight (framework page 6.3, D-B6) |
| "A thread a library owns" as the only non-loop thread (7) | status written by the code (`writer::rt`) or a loop of the spec; a library thread posts to the loop that owns the field | — |
| Diagnostic codes and texts: `ear.loop-slow`, `ear.loop-source-left` (6.4), `ear.claim-wait` (6.9), `ear.close-late`, `ear.close-overdue` with loops (6.10) | puts them on the wire (`node.diagnostic`) and in snapshots | done (control-plane.md 4.6) |
| Counters: a sink's paused frames (6.6) | samples them beside the framework's drops and shortfall (framework page 8.2) | done: per companion (control-plane.md 4.10) |
| `notes-dropped <n>` as the framework's note | `ear.events-dropped` diagnostic | — |
| `log` records with `node` from a node's handle; no generation in any record (framework page I6) | `log.subscribe` later | — |
| `TypeSpec`'s whole member list and order (6.13) | `.status`, `.events`, `.actions`, `.logCodes` after `.lifecycle` | done: cites 6.13 |
| The harness's fakes, by handle, and its clock rule (10) | captures status, events, actions and diagnostics; `h.invoke<action>(args)` on the harness's loops | — |

Not slice 3c's: `restart` and every action, status tables, the wire schema, `typeSchemas`, parameters (slice 6),
recovery (slice 8), the lifecycle's states on the wire (`phase`, `applied`, `companions`). A store does not appear on
the wire.

## 15. The cut: buildable steps

Each step keeps every test green and changes no audio path. Slice 3c's steps assume 3a and 3b built as the framework
page's 13.1 and 13.2 say; S3d.1 and S3d.2 come inside slice 3d, after its in-place path (`configure` with `old`, the
plan's outcomes) exists.

| Step | What | Tests it adds |
|---|---|---|
| **S3c.1 Node handle** | `NodeHandle` replaces `InsidesEvents` and `Channel`; `node.handle()`; `NodeHost::logTo`; `say` paced in core per node (sendspin's `NoteGate` stays until S3c.6, so its notes are paced twice, harmlessly); `LogEntry::run` goes | a node's log carries its node; a log after detach is dropped; a late message from an older generation dropped; a goodbye note is heard; pacing 32 then 1/s with `notes-dropped`, per node, kept across a stop and a start; `lost` never held back |
| **S3c.2 Registry and I/O thread** | `ResourceAdapters`; `IoThread` with shared-state jobs and its stop; `ear_refuse_node_type_links` refuses `ear_runtime` and `ear_pipewire` | a null adapter refuses its kind; a job's completion for a gone configuration dropped and its descriptor closed; the I/O thread busy at stop; the link refusal on each package |
| **S3c.3 Loops** | `loop.hpp`, `Loops` with state types, `TypeSpec::loops`, `StrandPool`, `DedicatedLoops`; `cx.loop`, `cx.executor`, typed posts (`NodeOf::post`, `cx.post`); staging until configured; `postFromOutside`, `closeToOthers`; teardown step 4 (posted work, then the state destroyed on its loop); the source count; `Loop::onWake` on `EventWake` (the source's played wake loses its interim `fd()`, U14); the harness's manual loops and clock | post order; `after`, `every` (coalescing), `onWake` (a pending wake fires once on arming), `onReadable`; sources armed and work posted from `configure` run only once configured, and never after a configuration that failed first; a state destroyed on its own thread; a post typed for another loop's state does not compile; queued work destroyed unrun at close; an in-flight handler finishes; outside work refused from step 1; a source left armed at 4b; a dedicated loop that never drains is leaked with "1 loops open"; `ear.loop-slow`; a stuck strand gets a replacement thread; the blocking assert on a strand; the harness's stop failing at once with nothing to turn; the wake's drain-then-clear order (harness hook and real-thread stress) |
| **S3c.4 Stores** | `StoreRule`, `Stores`, `TypeSpec::stores`, `NodeType::stores()`; `ConfinedStores` with sweep, open at readiness step 2, serialised `replace` and `replaceLater`, the 5 s deadlines | store open (missing, created, symlink, too big, distrusted with the file untouched, mode narrowed before judging); `replace` refused when changed; two writes in flight both commit in order; mode narrowed before a write; sweep over declared stores; open past its deadline and its late completion dropped; one compile test of every `TypeSpec` subset spelling (U1); compile-fail cases: two stores of one suffix, a bad suffix, `maxBytes` left out, a mode beyond 0777, `bx.store` and `bx.loop` of a handle the spec lacks |
| **S3c.5 Claims, recordings, consumers' `begin`** | `claim::TcpListen`, `claim::Udp`, `ClaimRegistry` at readiness step 1, `KernelPortProbe` (UDP without SO_REUSEADDR); implicit store, recording and RTP receiver claims; recordings deferred onto the I/O executor; `SinkConsumer::begin` and readiness step 4'; `file` opens its WAV in `begin`; the leaked-configuration record, late close, auto-start (R15); `ear.claim-wait`, `ear.close-late` in `output-records.json` | granted; refused (running holder, across types) before any open or thread; waited (a removed node on 8928 and an added one on 8928 in one plan; `rec` removed and `rec2` added on one path) with `ear.claim-wait`; the 15 s deadline; a claim held by another node's going old configuration (a restart's, a failure's) waits, so two nodes whose restarts swap ports both run, with their teardowns finishing in either order; an abandoned port re-probed; a store and a recording path refused while a leaked configuration holds them, then freed by its late close and the node running by itself (a system test: a peer that never answers at stop, its goodbye finishing after the deadline, the same node then running); a configuration leaked in step 3 has its loops closed; a configuration torn down while waiting leaves the queue; `begin` failing and `begin` past 5 s; probe cases moved; system tests for visible changes 2 and 3; `file`'s exact-samples and header tests unchanged |
| **S3c.6 sendspin moves** | section 12 (3c's column); `node_folders.hpp`, the folder argument and `notYetInHarness` go; the thread guard | `type_test.cpp` (3c's cases); sendspin in the all-type harness test; the guard on each spelling, a look-alike and the `/tests/` exclusion; every system test (U4) |
| **S3c.7 The log patch** | `cmake/sendspin-log.patch`; `sendspin_patched_dep` with a patch list; the worker's state sets the sink and context | `ear_carried_patch_check` for both patches; a library line from the worker and one from the sync thread arrive as `log` records with the node (U2); two nodes' lines told apart; a line outside any scope is unattributed from `sendspin-cpp` |
| **S3c.8 Words** | AGENTS.md (architecture, threads, adding a type, tests, sendspin's library row, `rtp`'s held-port line, `file`'s recording line), the framework page's "Slice 3c as built", the `sendspin` skill | — |
| **S3d.1 Primitives in an in-place change** (inside slice 3d, after `ConfigureOf`) | `cx.claim`, `cx.unclaim`, the node's own key, the claim wait against the change's deadline, the break-first rule for a same-port RTP receiver; `cx.recording`; `whenReady` and `ConfigureDone`; `SinkControl::pause` and `resume`; the source writer's hand-over check in debug builds; the harness's `change` with these | a claim swapped make-before-break (both held, the old released last); a refused new claim destroys the continuation and restarts; two nodes swapping ports both restart and both run; a same-key replacement granted without a probe and made break-first; a continuation runs only once every acquisition is ready, on the loop named; a token dropped fails the reconfigure; a stop before the post destroys it, after the post lets it run before the goodbye; a newer change before the post (T15) destroys it and releases its claim, after the post waits; pause waits for a consume in flight, discards and counts, posts `paused`; a paused sink at teardown finishes; a second writer without a hand-over asserts |
| **S3d.2 sendspin in place** (G19, decided (a); if the owner overturns it to (d), this step is skipped and sendspin's rows stay restart-only) | sendspin's rows marked `.inPlace()`; its in-place `configure`, `redial`, `swapClient` (4.2); the advertiser re-opened | `type_test.cpp`'s 3d cases; system tests: a listening node's port changed while it plays (`reconfiguring: port`, notes, same `id`, the key file not read again, the stream ended and Music Assistant's test server reconnecting to the new port, no `stopping`); a name changed (the server sees the new name after it reconnects, same id); a server URL changed (a redial, no new client); a change to a held port restarting and failing with today's text; dialling to listening and back (U11, U12) |

## 16. Costs and risks

- **A wider core and a new package's growth.** About 500 lines of interfaces, facades and registry in core, about 700 in
  `ear_runtime` (loops, I/O thread, probe, stores; mostly moved from sendspin), against about 450 fewer in sendspin after
  3b already removed its realtime code.
- **`ResourceRegistry`'s constructor changes**, so every `NodeHost` test's setup changes once (S3c.2).
- **Compile-time machinery grows**: stores and loops reuse the bank-name machinery; about ten compile-fail cases (about
  1.5 s each at one job).
- **One I/O thread.** A hung disk stalls every open and every store write; the 5 s deadlines make it visible, not
  harmless.
- **A shared strand pool** can let one node delay another for up to 1 s before a replacement thread starts; sendspin
  uses only dedicated loops, so nothing built runs on a strand yet.
- **Thread names do not change**: loops named `sendspin` and `notify` give `ear-sendspin <node>` and `ear-notify
  <node>`, as today (`worker.cpp:190`, `310`) [V].
- **A second carried patch** to rebase at each bump of the sendspin-cpp pin, touching about 15 places (U2).
- **Leaked configurations keep stores and paths for longer** than today (refused until a late close), so a node behind a library
  that never finishes its stop stays failed until ear restarts; a late close restarts it by itself (R15).
- **Claims are by port, not by address.** Two `rtp` nodes on one port with different multicast groups are refused,
  though PipeWire binds each to its group (`module-rtp-source.c:265-309`) [V]. Multicast is untested (AGENTS.md); a
  later key `udp:<group>:<port>` can relax it.
- **sendspin's in-place change is a new client.** Its connection drops and Music Assistant reconnects (seconds); what it
  saves against a restart is the key file's re-read, the loops' threads, the companions, the worker's state and its
  wires' audio from the source; an operator still hears the stream stop and start (U12).
- **A pause discards what the node sends** during a swap; a consumer that wanted the audio queued cannot have it.
- **A claim wait in an in-place change** can cost up to the change's deadline and then the restart's own 15 s, about
  25 s before the node fails.

## 17. Rejected alternatives

| Alternative | Why not |
|---|---|
| Loops on `pw_thread_loop` in `ear_pipewire` | A loop has nothing to do with the engine; strands need a pool `pw_thread_loop` does not give; every threaded type's tests would start PipeWire's loop library (G1) |
| The type's instance keeps owning its threads (no loop primitive) | Against Q5; each type rewrites stop, wake and poll |
| Loops asked for in `configure` by a runtime name | A name checked only at run time, and slice 5's `call<..., Loop>` checked only at start (G14) |
| A recipe lambda for spec types | Every threaded type repacks resources by hand and a dangling capture compiles (G15) |
| Kept values, a feed and a late handoff consumer (revision 2) | Made necessary only by the two-phase start (R13, R14; 3.1) |
| Staging released when `start()` returns (revision 3) | Work that only needs its resources (a state reading its store) would wait on the engine; what must wait for the code to drive is what `start()` posts |
| A close hook beside `stop()` (revision 3's `close(closed)`) | R23's three hooks; a loop's state destroyed on its own loop after the goodbye already posted needs no hook (D-A6) |
| A bare reference to a loop's state on the main loop (revision 3's run object, read by its hooks) | A data race a hook could write; typed posts make it impossible (framework page I14) |
| An in-place change that reports readiness by callback per kind, or a second hook `reconfigured(...)` | A callback per kind is plumbing every type writes; a second hook on the main loop cannot block, and sendspin's swap must run on its worker. `whenReady` is the change's continuation (G20) |
| The type's own lock around what its consumer writes into | Every in-place type writes it, and a mistake deadlocks the sink's thread against the type's loop; one framework hand-over is tested once (G21) |
| Replacing the sink companion to change what its consumer writes into | Two engine nodes and two rings for a moment, both fed, to move one pointer |
| Restart for sendspin's listening/dialling switch (the framework page's 6.6 estimate) | The library needs a new client for a name or port change anyway (its configuration is fixed when made, `client.h:333-335` [V]); a new client that dials costs nothing more (G19) |
| A same-key replacement probed like a new claim | Its own node holds the key, so the probe fails against itself |
| Wakes for types now (`wake::named`, revision 2) | No type rings one after 3b; a handle no type uses is machinery to test and keep (G23) |
| Stores opened by `configure` or a loop's state | I/O on the main loop, or a state that reads before the file is trusted |
| Asynchronous store writes only | The library's `save_blob` returns a `bool` synchronously on its thread (G7) |
| A recording claim judged inside `configure`, the open kept there | No wait is possible, so I8's remove-and-add of one path cannot work, and `file` keeps I/O on the main loop (G16) |
| Claims judged after stores open | The open would read, narrow or create the very file a leaked configuration still holds |
| Claims per type, as sendspin's | A port is ear-wide |
| The probe alone, no registry | Loses "held by node <n>", and refuses a remove-and-add of one port in one plan |
| A closed source writer that returns 0 at once to every caller | sendspin-cpp's sync thread paces itself on the sink blocking and would spin from step 1 to step 4 |
| Passing the client at each `SS_LOG*` site | 533 sites: a patch no bump could carry (G8) |

## 18. Gate questions

Revision 4 follows the owner's change of practice (framework page R24, 14.1): the owner is asked only about what an
operator or a type author sees, and every technical choice is decided by the designer and listed with its reason. Every
question this page asked was technical, so none is left to the owner; 18.3 lists each decision so the owner can overturn
any. The three pages' owner gates (revision 6 of the framework page and revision 4 of control-plane.md: the framework page's F1, F2 and C2 and control-plane.md's W3, X2,
G1, G3 and G4) were all answered on 2026-11-08 (framework page 14.1, Q26 to Q28; control-plane.md 14.1); none is open.

### 18.1 Answered, superseded or settled

| # | Question | Answer | Where it lands |
|---|---|---|---|
| G1 | Where do loops, wakes, the I/O executor, the probe and stores live? | The owner: (a), a new `ear_runtime` package | 11 |
| G14 | How does a type get its loops? | The owner: (a), declared in the spec as handles (revision 4: each with its state type) | 6.4 |
| G15 | How is the run object made? | The owner: (a), reached by handle, not a recipe; revision 4: inside `configure` (R19), with a loop's state where threads use it | 4.1, 6.1 |
| G17 | The run handle's accessor | The owner: (a); revision 4: `node.handle()` on the instance's `NodeOf` (R20) | 5 |
| G3 | A store, recording path or port held by a leaked run | The owner: (a), R15 (a leaked configuration) | 6.9, 6.10 |
| G2 | A mailbox: resource kind or kept value? | Superseded by R13: the instance or a loop's state owns it (framework page 6.5) | 3.1 |
| G4 | A handoff whose consumer is part of the run object | Superseded by R14: companions (framework page 9.6) | 3.1 |
| G6 | `notYetInHarness` | Settled by the framework page's 13.1: it goes in 3c | 10 |
| G10 | The run handle in slice 3 or 5 | Settled by the framework page's 13.1 and 13.2: the node handle, 3c | 5 |
| G20 | How an in-place change acts once its new resources are ready | Settled by R23 (no fourth hook): `cx.whenReady` (6.2) is the only way left that is neither a hook nor a callback per kind | 6.2 |

### 18.2 Owner gates

None on this page (see the paragraph above).

### 18.3 Decided by the designer

| # | Decision | Why |
|---|---|---|
| G18 | This page designs the primitives' part of slice 3d (claims swapped in place, `whenReady`, the sink's pause, sendspin's in-place `configure`); the framework page the rest | Each fact keeps one home: claims and loops are this page's in every state; the states and the contexts are the framework page's |
| G19 | sendspin's `name`, `port` and `server` change in place, the listening and dialling switch included (4.2). The audio drops either way, since the library needs a new client; in place keeps the node's configuration, its identity without re-reading the key file, and both ports claimed for a moment. If the client swap's unknowns (U11, U12) fail in slice 3d, the rows stay restart-only and S3d.2 is skipped | R17 names a network port change as the case a node handles itself; the machinery is built for `rtp` and the RTP sender anyway. The operator hears the same drop either way, so it is not a question of what they see |
| G21 | `SinkControl::pause` and `resume`: the framework hands a sink's consumer to a loop and back (6.6) | One hand-over, tested once, instead of a lock in every type |
| G22 | A replacement asking a key its own node holds is granted unprobed, and a same-port RTP receiver is replaced break-first by the framework, behind its own hold (6.9) | The type cannot know how PipeWire binds; the framework knows the key is the same. The gap is a fade, silence and a fade in, never a click |
| G23 | Types get no wakes of their own yet; `Wake` and `Loop::onWake` stay for the framework's (6.3) | Nothing would use them; the design is kept and costs one step to add |
| G16 | A recording's path is claimed first, opened on the I/O thread, its header written by the consumer's `begin` before the node is configured (6.11) | A rename on the same path works, a duplicate path is refused before anything is truncated, and the main loop never touches the disk |
| G13 | Opens 5 s; a claim wait 15 s (after the holder's own 10 s teardown deadline); a consumer's `begin` 5 s | Every wait added here gets an end, so a node never stays `starting` for good (framework page I3) |
| G7 | A store writes with `replace` (blocking up to 5 s, from a dedicated loop or a library thread) and `replaceLater` (any loop), queued in order on the I/O thread | sendspin-cpp's persistence callback answers synchronously on its own thread |
| G5, G11 | The node handle paces every type's `say` (32, then one a second, `notes-dropped <n>`); boot findings stay `log` records from the type's name until slice 5 | sendspin deletes `NoteGate` now; slice 5 replaces both texts |
| G8 | A carried sendspin-cpp patch adds a log hook with a per-thread context (section 9) | Small enough to rebase at each pin bump; worth offering upstream |
| G9 | The strand pool: `min(4, cores)` threads, slow at 100 ms, a replacement thread at 1 s, at most 16 | One stuck strand delays the others for at most 1 s; measured on the Talos CPU once known (U5) |
| G12 | A configure-time guard refuses `std::thread`, `std::jthread`, `std::async`, `pthread_create` and `fork` under `src/nodes/` outside tests (7) | A type's threads are its loops' (framework page D-A4), checked on every build as standard output already is. A type author meets it as "declare a loop" |

## 19. Open items

- **U1.** Aggregate deduction for `TypeSpec{.settings = ..., .banks = ..., [.stores = ...], [.loops = ...]}` with every
  later template parameter defaulted: S3c.4 adds one compile test of every subset spelling before any type uses them;
  slice 5 extends it. The fallback is a consteval `makeSpec(...)` with the same members in the same order.
- **U2.** That every `SS_LOG*` line sendspin's tests provoke runs inside a scope: S3c.7's system test counts
  unattributed `sendspin-cpp` records over the pairing and connection tests; each one found is a missed entry point.
- **U3.** That the library calls the persistence provider only on threads in the client's scope (`client.cpp:1648-1741`,
  `player_role.cpp:751-778`, `record_store.cpp:43` [V] are the call sites; their threads are not traced). S3c.6 asserts
  in `save_blob` that it runs on a dedicated loop or a library thread, never a strand.
- **U4.** sendspin's system tests and a `running` line before a corrupt key file's failure (8): S3c.6 runs them.
- **U5.** The strand pool's sizes on the Talos nodes' CPU, unknown until the hardware is (AGENTS.md).
- **U6.** Whether `onReadable` serves sendspin's mDNS socket better than answering in the 10 ms tick: not needed to move.
- **U7.** The writer's close contract (6.5) keeps sendspin-cpp's sync thread asleep between calls from step 1 to 4a:
  3b's source tests prove it on the ring (a caller looping on a closed writer uses under 1% of a core over 2 s); S3c.6
  measures the real sync thread across a teardown held 2 s in step 3 by a slow consumer.
- **U8.** Upstream interest in the log hook.
- **U9.** The boundary with control-plane.md: rewritten with its revision 4 and the framework page's revision 6 in one
  change (section 20). An owner's overturning of a designer's decision that moves a seam (G18, G19) changes both pages.
- **U11.** That a second `SendspinClient` made in the same process over the same persistence provider, after the first
  was stopped and destroyed, has the same client id and pairings. The docs say a `start()` after `stop()` keeps them
  (`client.h:347-348` [V]) and the client id is derived from the persisted keypair (`config.h:171-174` [V]); a new
  instance is not stated. S3d.1's `type_test` and S3d.2's system test check it.
- **U12.** How long a client swap takes: `stop()` is bounded by the transports' send and close
  (`client.h:351-354` [V]) and Music Assistant's reconnect is its own. S3d.2's system test measures it against the 10 s
  reconfigure deadline; sendspin may declare a longer one.
- **U13.** What Linux does with unicast datagrams for two sockets bound to one UDP port with SO_REUSEADDR (rtp-source,
  `module-rtp-source.c:252` [V]); the break-first rule of 6.9 makes the answer moot for ear, and S3d.1's RTP test shows
  no doubled audio.
- **U14.** That 3b ships the source's played wake with an interim `fd()` sendspin's notifier thread can poll before
  loops exist; if 3b waits for loops instead, S3c.3 has nothing to remove.
- **U15.** That the sink companion's thread can stop calling a consumer between chunks without a lock on the ring's hot
  path (an atomic flag read per chunk, a post once the in-flight call returned): S3d.1's test runs a writer at full rate
  while pausing and resuming 10^4 times.

## 20. Review record

### Revision 1, reviewed (round 1)

Revision 1 was reviewed by two reviewers: realtime and concurrency (RC1 to RC18) and DX and API (DX1 to DX17). Each was
checked against the code at `f677a50` before acting on it. The last column says what revision 3 kept.

| Finding | Outcome in revision 2 | In revision 3 |
|---|---|---|
| RC1 late handoff consumer misdates captured audio | fixed: `attached(skippedFrames)`, the atomic consumer (6.6); G4 | moot: no late consumer; a sink's chunks carry their own stamps (framework page 9.6) |
| RC2 a source can fire after `closed`; `after(0)` never cancelled; destruction rule contradicts step 5 | fixed: the source contract, `ear.loop-source-left`, `[[nodiscard]]` (6.4) | kept (6.4) |
| RC3 recording claims cannot be judged at readiness; `file` does I/O in `start` | fixed: recordings deferred; WAV in `attached` (6.11); G16 | kept; the WAV in `begin` (6.6, 6.11) |
| RC4 a closed feed and the lost stop-fd wait make the sync thread spin | fixed: the close contract and `idle` (6.5); U7 | kept as the source writer's contract (6.5) |
| RC5 the two pages disagree on `TypeSpec`'s order | fixed: one home | kept (6.13) |
| RC6 actions can run between 4a and 4b | fixed: `postFromOutside`, `closeToOthers` at step 1 | kept (6.4) |
| RC7 the late close has no mechanism and restarts nothing | fixed: the leaked-run record, `ear.close-late`, auto-start; G3 | kept; G3 answered (a), R15 (6.10) |
| RC8 claims judged after stores open | fixed: claims first | kept (6.1) |
| RC9 the wake's drain/clear order and a ring before arming | fixed (6.3) | kept for the framework's wakes (6.3) |
| RC10 a store open past its deadline; a hung I/O thread at stop | fixed (6.7) | kept |
| RC11 a UDP probe with SO_REUSEADDR misses rtp-source | fixed (6.9) | kept |
| RC12 `rtp` claims nothing; a failed holder | fixed | kept; the failed-holder row goes with A3 (3.1) |
| RC13 a claim wait gives no reason | fixed: `ear.claim-wait` | kept |
| RC14 a forgotten `attach`; implicit pass-through writes | fixed: never-attached throws; `toHandoff` | moot: consumers handed over in `build`; `toHandoff` gone |
| RC15 the rtpsend example races on its socket | fixed: one thread owns the socket | kept, with the pause as the second owner's hand-over (4.3) |
| RC16 the harness's close waits 10 s of real time | fixed: fails at once when idle | kept (10) |
| RC17 store writes are not serialised | fixed (6.8) | kept |
| RC18 small inconsistencies | fixed | kept where still relevant |
| DX1 recipe arming races; failed `readKeys` destroys a running source | fixed: staging; keys parsed first | kept; staging now ends at `start` (6.4) |
| DX2 recording claim after truncation | fixed (6.11) | kept |
| DX3 stores open before claims | fixed | kept |
| DX4 teardown of a run not yet ready; a failed start | fixed (6.10, 6.1) | kept; "a start that fails" is now "a `setup` that fails" |
| DX5 `Ready` and requests not tied to the spec | fixed: `StartOf`, `ReadyOf` | kept as `SetupOf`, `BuildOf` (6.1) |
| DX6 hand plumbing; loop names as runtime strings | fixed: spec handles; G14 | kept; G14 answered |
| DX7 the stop-eventfd wait has no replacement | fixed: `idle` | kept (6.5) |
| DX8 implicit pass-through to a handoff | fixed: `toHandoff` | moot |
| DX9 visible changes undercounted; claim key normalisation | fixed | kept; change 4 gone, change 5 added (8) |
| DX10 harness keyed by strings | fixed | kept (10) |
| DX11 the thread guard would match sendspin's tests | fixed: `/tests/` skipped | kept (7) |
| DX12 "a thread the run object owns" contradicts P2 | fixed | kept (7) |
| DX13 renames missing from section 3 | fixed | rewritten for revision 3 (3) |
| DX14 `keep<T>` takes any `T` | fixed | moot: kept values gone |
| DX15 `maxBytes` silently 0 | fixed | kept (6.8) |
| DX16 "no I/O" in `start` false for recordings | fixed | kept, for `setup` and `build` |
| DX17 `ready.run()` reads as a verb; ports narrowed by hand; pacing scope | fixed: `ready.handle()`; claims take checked numbers; per run | kept; G17 answered (`bx.handle()`) |

### Revision 2, boundary reconciled with control-plane.md revision 2

Each fact given one home and cited from the other page: control-plane.md is the home of the realtime calls (`io.set`,
`io.raise`, `io.log`), the ring the framework makes per run, `Report<spec>`, no `writer::thread`, the status counters,
and `log.subscribe` after slices 5 and 6; this page is the home of `TypeSpec`'s member list, slice 3's diagnostics and
texts, and the claim wait's spelling, `ear.claim-wait`. Revision 3 keeps the split; section 14 lists what
control-plane.md's own rebase is expected to change.

### Revision 3 (2026-11-08, not reviewed)

Written to the framework page's revision 5 (R11 to R18) and the owner's answers G1, G3, G14, G15 and G17.

- **Removed** (3.1): kept values and this page's mailbox; the feed; the handoff with a late consumer and `attached`;
  `RunCode::toHandoff`; type-asked wakes (deferred, G23); `StartOf` and `ReadyOf` and readiness step 0; visible change
  4; the failed-holder claim row.
- **Rebased**: every primitive placed in the lifecycle's states (6.1); readiness steps 1 to 3 kept, step 4' added
  (consumers begun); staging ended by `start` (6.4); teardown with companions at steps 2 and 3, and a partial teardown at
  a reconfigure's end (6.10); the run handle reached as `bx.handle()` and `rx.handle()`, kept across a reconfigure (5).
- **New**: `rx.whenReady` and `ReconfigureDone` (6.2); claims in a reconfigure (make-before-break, the run's own key,
  the deadline, break-first for a same-port receiver, 6.9); the source writer's single-writer hand-over (6.5);
  `SinkConsumer::begin` and `SinkControl::pause` (6.6); `rx.recording`; sendspin's `reconfigure` with its client swapped
  inside its run, all three settings in place (4.2, G19); the RTP sender reconfiguring its target (4.3); invariants P5
  and P6; steps S3d.1 and S3d.2.
- **Renumbered**: `TypeSpec`'s home is now 6.13 (was 6.12); steps are S3c.1 to S3c.8 and S3d.1 to S3d.2 (were S3.1 to
  S3.9). control-plane.md's citations of 6.12 and S3.x need the new numbers.
- **Gate questions**: G1, G3, G14, G15, G17 recorded as answered; G2, G4 superseded; G6, G10 settled by the framework
  page; G5, G7, G8, G9, G11, G12, G13, G16 asked again with background and scenarios; G18 to G23 new.

Its review should be given sections 3, 4.2, 6 and 18 and the framework page's sections 6 and 9.6, and judge at least:
whether every primitive's behaviour in `reconfiguring` keeps I4 to I8; the pause's hand-over on the sink's thread
(U15); the claim rules when a reconfigure, a restart and a removal of other nodes interleave; sendspin's client swap
against the library's threading rules (`client.h`); and whether the steps keep every test green between them.

### Revision 3, reconciled after the framework page's review round 3

The framework page's revision 5 went to two reviews (realtime audio, RA; API and developer experience, DX). Their
findings that touched this page, each checked in the code first:

| Finding | Outcome here |
|---|---|
| RA1: sinks armed after the drive barrier and never disarmed at teardown's | fixed: armed with the code's first drive, disarmed in the barrier's invoke (3.1, 6.1, 6.6, 6.10) |
| RA2: a break-first replacement not ordered against the hold's fade | fixed: the framework holds and waits for the fade's acknowledgement before destroying a same-port receiver (6.9, G22) |
| RA4: a sendspin rename does not keep the audio | fixed: 4.2 is the one home and says what in place buys and costs; G19 asked again with option (d); S3d.2 conditional on it |
| RA5: two nodes swapping ports: the second restart refused | fixed: "going" is any run between I5 steps 1 and 5, of any node (6.9); S3c.5's case in either order |
| RA8: a reconfigure cannot be overtaken | fixed here for the continuation: superseded before the post, which is the commit point (6.2, 6.10, 4.2) |
| RA11: blocking barriers | fixed: teardown step 1 continues from the invoke's completion (6.10) |
| DX1: the facades have no home | fixed: framework page 6.10 declares them whole; 6.1 lists this page's members and their rules; 3.3 redrawn |
| DX2: two homes for `SinkConsumer` | fixed: 6.6 is its home; the framework page cites it |
| DX3: a run lost after running is `start-failed` | fixed: `lost` fails `run-lost` after `running` (5) |
| DX4: `rx.run<T>()` on the main loop | fixed: `rx.post` and `rx.whenReady(loop, fn(Run&, token))` (4.2, 4.3, 6.2) |
| DX5: two calls to finish later | fixed: `whenReady` is the one later answer (6.2) |
| DX6: no spec member for the deadline | fixed: `.lifecycle` in 6.13 |
| DX11: consumers by raw reference | fixed: `Built::consume(bank, &Run::member)` or `consume(bank)` for a run object that is the consumer (4.3) |
| DX15: control-plane.md cited 6.12 for `TypeSpec` | fixed there; 6.13 is the home |

### Revision 4 (2026-11-08, not reviewed)

Rebased onto the framework page's revision 6 (the owner's R19 to R24): no run. The run handle is the node handle (5),
with no generation on records; `setup`, `build`, the run object's `start` and `close` become one `configure`, `start()`
and `stop()` (4.1, 4.3); what a type's threads use is a loop's state, declared with the loop's handle, given in
`configure` and reached only by typed posts (4.1, 6.4); staging ends when the node is configured, and teardown's steps
4a and 4b become one step 4 in which a loop runs what was posted and destroys its state on its own thread (6.4, 6.10); a
stop keeps every primitive (6.1, 6.6, 6.10); `whenReady` and `ConfigureDone` belong to an in-place `configure` (6.2).
Every open gate was technical and is now decided by the designer (18.3), G20 settled by R23. A review is owed with the
framework page's; it should judge at least the loop state's ownership against I14 (a state that leaks a reference to
the instance), the sink consumer as a loop state's member across a pause, and the interim for sendspin's threads in 3a.

### Revision 5 (2026-11-08, not reviewed)

Follows the framework page's revision 7 (the owner's R25 to R28): a `stopped` node keeps every primitive, unchanged
from revision 4; the new `disabled` state releases every primitive by the full teardown and keeps the block (6.1, 6.10),
holds no claim, so another node may take its port and an enable judges its claims afresh (6.9), and has `disable()` and
`enable()` in the harness (10). No decision of 18.3 changes, and no owner gate is open.
