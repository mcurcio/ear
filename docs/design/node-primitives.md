# Node primitives: slice 3c of the node framework

Status: proposal, revision 3, 2026-11-08. Revision 2 followed two independent reviews (section 20); revision 3 rebases
the page onto `docs/design/node-framework.md` revision 5 ("the framework page" below). The framework page's review round
3 (realtime audio, and API and developer experience) covered this page's boundary with it, and this text answers the
findings that touched it (section 20, last subsection). Nothing here is built.
Layer: module design, beneath the framework page. It designs that page's slice 3c (primitives: the run handle, loops,
the I/O executor, stores, claims, recordings, the carried log patch, `sendspin` onto the type interface) and the part of
slice 3d that is a primitive's (claims swapped make-before-break, a reconfigure that goes on on a loop, a sink paused
while a library is swapped, `sendspin` and the RTP sender reconfiguring in place). The node lifecycle itself (states,
transitions, holds, the code swap, reshape, companions) is the framework page's sections 6 and 9.6; the hook facades
(`SetupOf`, `BuildOf`, `Built`, `ReconfigureOf`, `Reconfigured`) are declared whole in its 6.10, members of this page's
included, and this page is the home of those members' rules; section 3.3 draws the line. Slices 5 and 6 are
`docs/design/control-plane.md` (revision 3, reconciled in the same round); section 14 is the boundary, and 6.13 is the
one home of `TypeSpec`'s member list for every page.

[V] = verified in the file named, as of `3c87691` (the code is unchanged since `f677a50`); [U] = unverified, with the
step or test that checks it (section 19). Files of the pinned sendspin-cpp tree (`7be2278`, `.cache/deps/sendspin-src`)
are cited by their path in it; PipeWire 1.4.2's source as `pipewire-1.4.2/<path>`.

## 1. The owner's words

| # | The owner's words | Date, where | What it means here |
|---|---|---|---|
| R1 | "what are the core primitives that ear should host so that new nodes can be added simply and only require defining the truly custom bits that its node type demands" | 2026-11-07, chat | The primitives sendspin wrote for itself become the framework's; a type writes its protocol |
| R9 | "Nodes should not have direct access to stdout or stderr. The core framework should expose the necessary primitives for events, diagnostics, and logging. These should be real-time safe for any audio loops that need them, and there can be non real-time bases for the rest of the system." | 2026-11-07, chat (gate, Q1) | A run's threads log through its handle; the library's own lines are attributed to the node (section 9) |
| Q5 | "Strands, dedicated on request" | 2026-11-07, chat (gate) | Loops (6.4) |
| Q7 | "Pipe into a feed first" | 2026-11-07, chat (gate) | The paced source of slice 9 builds on the source writer's contract (6.5) |
| P1 | "3 seems like a standalone unit, while 5 and 6 need to be designed together." | 2026-11-08, chat | This page stands alone; it fixes only the seams slice 5 extends (section 14) |
| P2 | "Let's start the design and architecture phase for both. I want a complete proposal or a design doc before implementation. The goal is high UX and DX" | 2026-11-08, chat | DX is what a type author writes (section 4); UX is what an operator sees (section 8) |
| R11 | "a Run should not be bounded by config changes. I want config and graph changes to be supported at runtime while audio is running" | 2026-11-08, chat (framework page R11) | Every primitive here survives an in-place reconfigure: loops, stores, the run handle and unchanged claims are the run's, not the settings' (6.1) |
| R12 | "the start+build model seems overly simplistic. I think each node deserves a dedicated state machine, and hooks for each viable state. At a minimum, there should be setup+build+start" | 2026-11-08, chat (framework page R12) | Each primitive is acquired in `setup`, readied before `build`, reached in `build`, set going in `start` (6.1) |
| R13 | "mailboxes could be supportable in either mode. But why must mailboxes be built before the run object exists? Is this a side effect of the naive node state machine model I described?" | 2026-11-08, chat (framework page R13) | Yes; revision 2's kept values go (3.1) |
| R14 | "G4 is asking the wrong question. The node audio threads should be concerned with reading inputs and writing outputs. The framework should provide the necessary primitives to connect inputs/outputs across nodes AND anytime a node needs to cross inputs/outputs to non-realtime code (eg pipe outputs through a mailbox?)" | 2026-11-08, chat (framework page R14) | Revision 2's feeds and handoffs, which the code touched, go; a type's threads meet audio only as a companion's writer or consumer (6.5, 6.6) |
| R15 | G3: "(a)" | 2026-11-08, chat (this page's G3) | A store, recording path or port held by a leaked run is refused with a detail saying the node starts when that run closes; the late close starts it (6.9, 6.10) |
| R16 | "I like the basic shape of the state machine proposal. And most importantly: it should be per-node. Some node stopping/reconfiguring/failing does not effect the graph globally" | 2026-11-08, chat (framework page R16) | Every primitive is per run except the I/O executor and the strand pool, which are shared and bounded (6.4, 6.7) |
| R17 | "I want the node to control whether settings can be changed in place, with the framework supporting it. For example, changing a network port on a node is something that a node can handle on its own — even if it must output null audio frames for a few seconds. But if reconfiguring changes the shape of the input/output bus, then that might not be fully transparent" | 2026-11-08, chat (framework page R17) | A port change is the worked example: claims make-before-break (6.9), a continuation on the run's loop (6.2), `sendspin`'s client swapped inside its run (4.2) |
| R18 | "crossing to a non-rat [non-realtime] thread could be managed by allowing a node to internally pipe its inputs/outputs to a hidden node, to maintain the integrity and consistency of the processing graph" | 2026-11-08, chat (framework page R18) | Sources and sinks are companions (framework page 9.6); this page owns only what a type's threads see of them (6.5, 6.6) |

The owner's answers to revision 2's gate questions, 2026-11-08, stand: **G1** (a), a new `ear_runtime` package; **G14**
(a), loops declared in the spec as handles; **G15** (a), a static `build` reaching resources by handle; **G17** (a),
the run handle as `ready.handle()`, now `bx.handle()`; **G3** (a), R15 above. **G2** (a mailbox as a kept value) and
**G4** (a handoff whose consumer is attached late) are superseded by R13 and R14 (framework page 14.1, Q15 and Q16).
Design rules 1 to 3 of `pipewire-graph.md` hold, as do the framework page's I1 to I13.

## 2. The problem, measured

What remains for slice 3c once slices 3a (the lifecycle) and 3b (companions) are built, as the framework page's 13.2
plans them. Today's code is [V]; what 3a and 3b leave is the framework page's plan.

| Seam | Where today | After 3a and 3b |
|---|---|---|
| `sendspin` derives from `NodeTypeOf` and writes its own `NodeConfig` | `sendspin_type.cpp:72-130` | still: 3a moves its run object into `build` and its threads into `start`, but on its own `NodeConfig` (framework page 13.2) |
| `NodeFolders` and `makeSendspinNodeType(const NodeFolders&)` | `node_folders.hpp:12-18`, `sendspin_type.cpp:135-144` | still: its key files and the boot sweep |
| The run's way out is `InsidesEvents` through `Channel` | `node_type.hpp:171-180`, `node_host.cpp:168-226` | still, reached as `BuildContext::events()` (framework page 13.2) |
| Claims live in sendspin's type, across its nodes only | `port_claims.hpp:12`, `sendspin_type.cpp:83-92` | still; `rtp` claims nothing, nothing claims a recording path |
| A leaked run keeps its port or file while the next run starts | `node_host.cpp:446-464` | still |
| `file` opens its recording on the main loop and writes the WAV header there | `node_host.cpp:126-141`, `file.cpp:65-79` | the open still in `setup` or `build` on the main loop (3a moves the call, not the I/O) |
| `notYetInHarness = {"sendspin"}` | `test/nodes/all_types.hpp:71-74` | still |
| sendspin's threads: the worker (a 10 ms `poll()`), the notifier, their stop `EventFd` | `worker.cpp:93`, `200`, `278-299`, `309-323` | still its own `std::thread`s, started in `start` |
| Its key file: named, 0600, read, trusted, created, swept at boot | `key_store.cpp:31-97`, `sendspin_type.cpp:137-144` | still its own |
| Note pacing (32, then one a second) | `note_gate.cpp:11-12` | still its own |
| The library's log lines are unattributed `stderr` records | `worker.cpp:218`; sendspin-cpp `src/platform/logging.h:56-63` | still |
| Every settings change restarts the run: a `sendspin` port or name change closes the connection and re-reads the key file | `desired_graph.cpp:159-160`, `node_host.cpp:79-87` | 3d adds the reconfigure path; what `sendspin` needs to use it is here |

What 3a and 3b remove from revision 2's list [V for today's code]: `Feed` (`feed.hpp`, `feed.cpp`, 109 lines), the two `Mailbox`
members and the capture anchor (`worker.cpp:360-361`, `sendspin_code.cpp:25-26`), the bell (`worker.cpp:358`),
`SendspinCode` (`sendspin_code.cpp`, 46 lines) and the handoff to the server (`sendspin_type.cpp:100`) become a source
and a sink companion and their writer and consumer (framework page 9.6). Revision 2 designed kept values, a feed and a
late handoff consumer for exactly these; they go (3.1).

Three facts shape what is left:

- **The run object now exists before the code drives** (framework page T3, T4). So nothing a type shares has to be made
  before it; every primitive here is either the framework's (loops, stores, claims, the I/O executor) or the run
  object's own.
- **`ear_core` may not touch threads, files or sockets** (AGENTS.md, Architecture). Loops, stores, the probe and the I/O
  thread are interfaces in core and adapters in `ear_runtime` (G1).
- **A run outlives a reconfigure** (framework page 6.6). A primitive bound to the run (a loop, a store, the run handle)
  must be right for every settings value the run will take, and a primitive bound to a setting (a claim on a port, a
  recording on a path) must be replaceable make-before-break.

## 3. Decisions, and where this page changes the framework page

In one paragraph: a type's spec declares its **stores** and **loops** as compile-time handles beside its banks. Its
`setup` asks for **claims** and **recordings** (and the companions of the framework page). The framework judges claims
first, then opens stores and recordings on its **I/O executor**, then starts loops; `build` reaches each by handle and
gets the **run handle**; `start` arms the run's loop sources, which run only once it returned. A run keeps its loops,
stores and handle through every in-place reconfigure; a reconfigure replaces claims and recordings make-before-break and
may go on on one of the run's loops once its new acquisitions are ready (`rx.whenReady`). A type's threads meet audio
only as a source's writer (one at a time, closed at teardown step 1) or a sink's consumer (begun before `running`, paused
while the type swaps what it writes into). A leaked run keeps listening, and its late close starts the nodes its claims
refused (R15). Implementations live in `ear_runtime`. `sendspin` moves onto `TypeSpec` and `TypeHarness`, and changes
its name, port and server in place by swapping its library client inside its run.

### 3.1 What revision 5 makes unnecessary

| Revision 2 had | Why it is gone |
|---|---|
| `cx.keep<T>()`, `ready.kept<T>()`, `KeptValue` (6.2) | Kept values were memory made before the run object because the code drove first (R13). The run object owns what it shares with its code (framework page 6.5, I13) |
| `ear/core/mailbox.hpp` as this page's (6.2) | The framework page's 6.5 moves `Mailbox<T>` into core as a plain class a run object holds; it is not a primitive of this page and no built type needs it after 3b |
| The feed (6.5: `cx.feed`, `FeedReader`, `FeedWriter`, `RingFeeds`, its latency declaration) | A source companion replaces it (framework page 9.6). Its writer's closing contract stays here, its home (6.5) |
| The handoff with a late consumer (6.6: `cx.handoff`, `ready.attach`, `attached(skipped)`, the atomic consumer pointer) | A sink's consumer is a member of the run object, named in `Built::consume` (framework page 6.10), and the sink is armed in the invoke that first drives the code and disarmed in the one that stops it, so it delivers exactly the cycles the code drove (framework page 6.5, 9.6, I4; R14) |
| `RunCode::toHandoff` | A type that only takes audio out passes its bank to an inside bank with a sink (`passThrough`), so the framework writes the ring |
| `wake::named`, `cx.wake`, `ready.wake` for types | No type rings a wake after 3b: the source companion rings its own played wake. `Wake` and `Loop::onWake` stay, for the framework's wakes (6.3; G23) |
| The two-phase seams: `StartOf<spec>` acquiring and returning the code, readiness step 0 ("the code drives at once"), "a start that fails frees handoffs, feeds and kept values", staging released when `build` returns | `setup` acquires and returns no code; `build` makes the run object and the code; nothing drives before T4. Staged loop sources are released when `start` returns (6.4) |
| Visible change 4 (`file` loses its first milliseconds) | The sink is armed with the code's first drive at T4, once its consumer has begun (6.6); nothing is discarded |
| A claim held by a failed run that was not torn down (6.9's "which failed" row) | A failed run is torn down at once (framework page 6.2, gate A3), so its claims are released. The row returns only if the owner answers A3 (b) |

### 3.2 Where this page refines the framework page

Each refinement is a module-level detail this page is the home of; the framework page keeps the contract it states.

| Framework page | Refinement | Here |
|---|---|---|
| 6.4, readiness | Its steps 1 to 3 and 4' are this page's (claims, stores and recordings, loops; after `build`, every sink consumer has begun, 5 s); the framework page lists them and cites this page | 6.1, 6.6 |
| 6.4, "loop sources staged since build" | Sources armed in `build`, in the run object's constructor or in `start` are staged; released in the order made once `start` returned. `start` is where a type arms them | 6.4 |
| 6.6, the hook's contract | `rx.whenReady(loop, work)`: the reconfigure's continuation, posted to one of the run's loops once every new acquisition is ready, with the run object and the `ReconfigureDone` token; the one way to finish later (the framework page's 6.10 has no `later()`) | 6.2 |
| 6.6, step 3 and "Break before make" | A replacement asking a claim key its run already holds is granted at once, not probed; an RTP receiver whose port is unchanged is replaced break-first by the framework, without the type asking, behind the framework's own acknowledged hold | 6.9 (G22) |
| 6.6, step 5 (deadline) | A claim wait inside a reconfigure counts against the reconfigure's deadline, not its own 15 s; the fallback restart waits afresh | 6.9 |
| 6.6, the built-in types' table, `sendspin` row | This page is the row's home (the framework page cites it). All three rows in place, the listening/dialling switch included: sendspin-cpp's configuration is fixed when the client is made (`include/sendspin/client.h:333-335` [V]), so a name or port change needs a new client anyway, and a new client may as well dial; the run, its key store, its loops and its companions stay; the connection and the audio do not (G19 puts what that buys to the owner) | 4.2 (G19) |
| 9.6, `SinkConsumer` | This page declares `SinkConsumer` (with `begin()`, once, on the sink's thread, before `running`) and `SinkControl` (`pause`, `resume`) whole, their one home; the framework page keeps the sink's realtime contract | 6.6 (G16, G21) |
| 9.6, the source writer | One writer at a time, handed over only by a join or a loop post; its closing contract | 6.5 |
| 7.3, Wake row | Types do not ask for wakes yet | 6.3 (G23) |
| 7.3, Recording row | A deferred kind: claimed at step 1, opened at step 2, reached in `build` as a descriptor | 6.11 |
| 8.8's erratum | The run handle and its node-facing `log` are slice 3c's; the boot sweep's findings stay `log` records from the type's name until slice 5 | 5, 6.8 |
| 12 ("the loop pool, eventfds and stores are behind interfaces, in `ear_runtime`") | `ear_runtime` links `ear_folder`; it exists from 3b (the sink's thread), and 3c adds loops, the I/O thread, the probe and stores to it | 11 |

### 3.3 The boundary with the lifecycle

| The lifecycle's (framework page 6, 6.10 and 9.6) | This page's |
|---|---|
| The states, the transitions T1 to T15, changes mid-transition and supersession | What each primitive does in each state (6.1's table) |
| `SetupOf`, `BuildOf`, `Built`, `ReconfigureOf`, `Reconfigured`, declared whole in framework page 6.10 (every member, this page's included), and their companion calls (`soundCard`, `rtpReceiver`, `source`, `sink`, `Built::consume`) | The rules of its members: claims, recordings, stores, loops, the handle, `whenReady` and `ReconfigureDone`, the source writer and `SinkControl` (5, 6.1, 6.2, 6.5, 6.6, 6.8, 6.9, 6.11) |
| Readiness's order and step 3' (companions); T4's invoke and `start` | Readiness steps 1 to 3 and 4' (6.1) |
| Holds and their acknowledgement, the code swap, reshape, the plan's outcomes, the reconfigure deadline (`Lifecycle`) | — (6.13 places the `.lifecycle` member) |
| I5's steps as a contract; companions at steps 2 and 3 | Each primitive's place in every step, full and partial (6.10); the leaked-run record and late close (6.10) |
| The ring companions' realtime side and their thread | The writer's and consumer's contracts as a type's threads see them (6.5, 6.6) |
| The harness's class and lifecycle steps (framework page 7.6) | The harness's loop, store, recording, claim and handle fakes and its clock (section 10) |

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

**After** (slices 3a to 3d). The spec declares the banks, the key file and the two loops. `setup` acquires; `build`
constructs; the run object's `start` arms its loops; `reconfigure` hands a change to the worker. No line carries a
resource from one hook to another: `build` reaches each by its handle, and the settings are every hook's argument.

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
inline constexpr auto worker = loop::dedicated<"sendspin">;   // the library's main loop: start, loop(), every callback
inline constexpr auto notifier = loop::dedicated<"notify">;   // apart: a late notify costs a hard sync
inline constexpr TypeSpec spec{.settings = SettingsTable<rows::server, rows::port, rows::name>{"sendspin"},
                               .banks = Banks<to, from, toServer, fromServer>{},
                               .stores = Stores<keyFile>{}, .loops = Loops<worker, notifier>{}};
using Checked = CheckedOf<spec>;

struct Sendspin {
	using Run = SendspinRun;   // framework page 6.10: build returns Built<Run>; reconfigure reaches it on its loops
	static Members<spec> ports(const Checked&) {
		return {to(members("left", "right")), from(members("left", "right")),
		        toServer(members("left", "right")), fromServer(members("left", "right"))};
	}
	static SetupResult setup(SetupOf<spec>& sx, const Checked& s);
	static BuildResult<Run> build(BuildOf<spec>& bx, const Checked& s);
	static Reconfigured reconfigure(ReconfigureOf<spec, Run>& rx, const Checked& was, const Checked& now);   // 4.2
};
}

// src/nodes/sendspin/sendspin.cpp
SetupResult Sendspin::setup(SetupOf<spec>& sx, const Checked& s) {
	if (!s.get<rows::server>()) sx.claim(claim::TcpListen{s.get<rows::port>()});   // judged first (6.9)
	sx.source(fromServer, {.frames = feedFramesFor(sx.format())});   // a companion (framework page 9.6)
	sx.sink(toServer, {.frames = sx.format().rate / 2});
	return {};
}
BuildResult<Sendspin::Run> Sendspin::build(BuildOf<spec>& bx, const Checked& s) {
	// The store's rule already trusted the text, so parsing it cannot fail; no I/O here.
	KeyRecord record = KeyRecord::parseTrusted(bx.store(keyFile).text());
	auto run = std::make_unique<SendspinRun>(bx, settingsFrom(bx.node(), s), std::move(record));
	return Built<Run>{std::move(run), passThrough({bankPair(to, toServer), bankPair(fromServer, from)})}
		.consume(toServer, &SendspinRun::toServer_);   // a member: begun on the sink's thread before `running` (6.6)
}
std::unique_ptr<const NodeType> makeSendspinNodeType() { return makeNodeType<sendspin::spec, sendspin::Sendspin>(); }
```

The run object takes what it uses from `bx` in its constructor and arms nothing there. `start` arms its loop sources;
they run once `start` returned (6.4). `close` lets the notifier go first, then the worker stops the client and calls
`closed`, having destroyed every source it armed:

```cpp
SendspinRun::SendspinRun(BuildOf<spec>& bx, SendspinSettings settings, KeyRecord record)
	: settings_(std::move(settings)), handle_(bx.handle()), keys_(bx.store(keyFile), std::move(record)),
	  played_(bx.source(fromServer)), toServer_(bx.format().rate, bx.sink(toServer)),
	  worker_(bx.loop(worker)), notifier_(bx.loop(notifier)) {}
void SendspinRun::start() {
	starting_ = worker_.after(std::chrono::milliseconds{0}, [this] { startClient(); });   // make, start, listen or dial
	tick_ = worker_.every(std::chrono::milliseconds{10}, [this] { tick(); });            // client.loop(), watch, mDNS
	notified_ = notifier_.onWake(played_.playedWake(), [this] { notify(); });           // notify_audio_played
}
void SendspinRun::close(std::move_only_function<void()> closed) {
	notifier_.post([this, closed = std::move(closed)]() mutable {
		notified_.reset();                  // on its own loop: no notify runs after this line
		worker_.post([this, closed = std::move(closed)]() mutable {
			starting_.reset();              // a client not started yet is never started
			tick_.reset();
			stopClient();                   // client->stop(): goodbye, its threads joined; the client destroyed
			closed();
		});
	});
}
// The library's sync thread writes the source companion (framework page 9.6), as it writes the feed today:
std::size_t SendspinRun::on_audio_write(std::uint8_t* data, std::size_t length, std::uint32_t timeoutMs) {
	if (!streamAsked_.load(std::memory_order_acquire)) {
		played_.idle(std::chrono::milliseconds{timeoutMs});   // sleeps its patience, before or after the close (6.5)
		return 0;
	}
	/* as today: convert, then played_.write(scratch_, frames, std::chrono::milliseconds{timeoutMs}) */
}
```

What the author no longer writes: a thread, a poll loop, an eventfd, a mailbox, a feed, a capture anchor, realtime code,
note pacing, a claims map, a probe, a `HeldPort`, a key file's open, mode or sweep, a `NodeConfig`, a `NodeTypeOf`, a
latency declaration, a folder parameter, or a lambda that carries resources between hooks. What stays: settings, the
protocol callbacks, the key record's format, mDNS, pairing notes (until slice 5), and the client swap of 4.2.

### 4.2 Reconfiguring in place: sendspin changes its port

The owner's example (R17): a network port changed while audio runs, which the node handles on its own. Scenario: node
`kitchen` listens on 8928 and Music Assistant streams to it; the operator sets `port 8929;` in the editor.

What the type writes:

```cpp
Reconfigured Sendspin::reconfigure(ReconfigureOf<spec, Run>& rx, const Checked& was, const Checked& now) {
	SendspinSettings next = settingsFrom(rx.node(), now);
	if (was.get<rows::server>() && now.get<rows::server>() && settingsFrom(rx.node(), was).name == next.name) {
		rx.post(worker, [url = *next.server](SendspinRun& run) { run.redial(url); });   // connect_to: no new client
		return Reconfigured::applied();
	}
	if (!was.get<rows::server>()) rx.unclaim(claim::TcpListen{was.get<rows::port>()});   // released at the end
	if (!now.get<rows::server>()) rx.claim(claim::TcpListen{now.get<rows::port>()});     // judged now (6.9)
	return rx.whenReady(worker, [next = std::move(next)](SendspinRun& run, ReconfigureDone done) mutable {   // 6.2
		run.swapClient(std::move(next), std::move(done));
	});
}
```

The run object is reached only on its worker (`rx.post`, `rx.whenReady`; framework page 6.10), typed by the
behaviour's `using Run = SendspinRun;`, so the hook cannot race the worker on the main loop. A redial is `connect_to`
on the running client (`client.h:389` [V]): it keeps the client, but drops the connection to the old server, so the
stream ends too.

`swapClient` runs on the worker, the library's main loop, which `start()` and `stop()` require
(`include/sendspin/client.h:337`, `362`) [V]. It is `close` without `closed`, then `startClient` with the new settings:

1. Posts to the notifier, which destroys its `onWake` source (so no `notify_audio_played` reaches the old client's
   player), and back to the worker.
2. `toServer_.pause(worker_, ...)`: the sink's thread finishes any `consume` in flight and then discards chunks until
   `resume`, and posts back once no `consume` can run (6.6), so `write_audio` never reaches a client being destroyed.
3. Says goodbye on mDNS (`mdns_goodbye_multicast`, `advertiser.cpp:257`), then `client->stop()`: goodbye to the server,
   the library's threads joined, `on_stream_end` delivered (`client.h:339-346`) [V], which clears the source; the
   client destroyed. The old sync thread, the source's writer, is joined, so the new one may write (6.5).
4. Makes the new client over the same key store, so the identity and pairings stay (U11), with `log_context` the same
   run handle (section 9); `start()`; on `false`, `done.fail("the Sendspin client did not start")`.
5. Re-arms the notifier's `onWake`, `toServer_.resume()`, opens the advertiser on the new port, and calls `done.done()`.

What the framework does around it (framework page 6.6 is the home of the steps; these are the primitives' parts):

| When | The framework | The operator sees |
|---|---|---|
| The commit | the plan's outcome is `reconfigure` (every changed key `.inPlace()`, ports unchanged); calls the hook on the main loop | `node kitchen reconfiguring: port` |
| The hook returns | judges `tcp:8929` (6.9): free, so granted and probed; no hold asked: the source plays what it holds until the stream ends, then fades out (framework page 9.6) | — |
| Every new acquisition ready | posts the continuation to the worker with the token (6.2) | notes `disconnected` and `stream-ended` from the old client |
| `done.done()` | releases `tcp:8928`, last; the node is `running` | notes `listening 8929 as ear-kitchen-<id>`, `id <same client id>`; `node kitchen running` |
| Music Assistant finds it again | — | `connected "Music Assistant" paired` |

**What in place buys here, and what it does not.** The audio stops either way: the old client says goodbye, the
stream ends, and Music Assistant reconnects to the new port (or the new server). Against a restart, in place keeps the
run (its number, its status, its loops and companions), the identity without reading the key file again (no disk
I/O, and no failure from a file that changed meanwhile), and both ports claimed for a moment so no other node takes
the old one mid-change; the operator sees `reconfiguring: port` instead of `restarting: port`. It costs `swapClient`,
the sink's pause (6.6) and two unknowns (U11, U12). G19 puts this to the owner.

Its failures, each a fallback to a restart with the new settings (framework page 6.6, gate B4):

- **8929 held by another running node**: refused at once; the hook's continuation is destroyed unrun; the restart's
  `setup` claims 8929 again and is refused: `node kitchen restarting: claim refused`, then `failed start-failed: cannot
  listen on port 8929: held by node porch`.
- **8929 held by a node still closing**: the claim waits with `ear.claim-wait`, against the reconfigure's 10 s deadline
  (6.9); past it the restart's claim waits afresh. A newer edit meanwhile (the operator types 8930) supersedes the
  reconfigure before its continuation is posted (framework page T15): the wait for 8929 is dropped and the hook is
  called again for 8930.
- **The new client does not start**: `done.fail(...)`, then the restart; a second failure is `failed` and recovery
  (slice 8).
- **Stop or removal during the swap**: a continuation not yet posted is destroyed unrun; one already posted is the run's
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
inline constexpr auto control = loop::strand<"control">;
inline constexpr TypeSpec spec{.settings = SettingsTable<rows::address, rows::port, rows::sourcePort, rows::channels>{"rtpsend"},
                               .banks = Banks<send, wire>{}, .stores = Stores<ssrcFile>{}, .loops = Loops<control>{}};

/// One thread owns the socket at a time: the sink's (begin, consume, finish), or the control strand while the sink is
/// paused (retarget). The pause's post is the hand-over (6.6), so nothing else synchronises.
class Sender final : public RunObject, public SinkConsumer {
public:
	Sender(const CheckedOf<spec>& s, BuildOf<spec>& bx, std::uint32_t ssrc);
	void begin() override;                     // socket, bind sourcePort, connect; failure: handle_.lost(...)
	void consume(const Chunk& chunk) override; // RTP timestamp from chunk.position; RTCP every 5 s of audio
	void finish() override;                    // BYE, close the socket
	void retarget(Ipv4 address, std::uint16_t port, ReconfigureDone done) {   // on the control strand
		sink_.pause(control_, [this, address, port, done = std::move(done)]() mutable {
			reconnect(address, port);            // connect(2) on a UDP socket: never blocks
			sink_.resume();
			done.done();
		});
	}
private:
	RunHandle& handle_;
	SinkControl& sink_;
	Loop& control_;
	int socket_ = -1;
	std::uint32_t ssrc_;
};

struct RtpSend {
	using Run = Sender;                                     // the run object, and the sink's consumer itself
	static Members<spec> ports(const CheckedOf<spec>& s) {
		const auto n = s.get<rows::channels>();
		return {send(members(n)), wire(members(n))};
	}
	static SetupResult setup(SetupOf<spec>& sx, const CheckedOf<spec>& s) {
		sx.claim(claim::Udp{s.get<rows::sourcePort>()});     // takes the checked number; outside 1-65535: logic_error
		sx.sink(wire, {.frames = sx.format().rate / 10});
		return {};
	}
	static BuildResult<Run> build(BuildOf<spec>& bx, const CheckedOf<spec>& s) {
		auto sender = std::make_unique<Sender>(s, bx, parseSsrc(bx.store(ssrcFile).text()));
		return Built<Run>{std::move(sender), passThrough({bankPair(send, wire)})}.consume(wire);   // the run object
	}
	static Reconfigured reconfigure(ReconfigureOf<spec, Run>& rx, const CheckedOf<spec>&, const CheckedOf<spec>& now) {
		return rx.whenReady(control, [a = parseIpv4(now.get<rows::address>()),
		                              p = now.get<rows::port>()](Sender& sender, ReconfigureDone done) mutable {
			sender.retarget(a, static_cast<std::uint16_t>(p), std::move(done));
		});
	}
};
}
```

The SSRC file is `rtpsend-<node>.ssrc` (I9), created with `newSsrc()` on first start, narrowed to 0644 at boot and at
each open; a corrupt one fails the node `start-failed: rtpsend-<node>.ssrc: malformed: ...` and is not replaced. A
second node on the same source port fails `start-failed: cannot bind UDP port 5006: held by node <n>`; removing one node
and adding another on that port in one plan works, the new one starting once the old one let go (I8), with `node <new>
ear.claim-wait: udp:5006 held by node <old>, which is closing` while it waits. A changed address loses the audio of the
pause (a few milliseconds, counted as paused frames, 6.6) and keeps the SSRC; the receiver sees a jump in RTP timestamp,
which RTP allows.

## 5. The run handle

**Decision: the run handle is slice 3c's** (the framework page's 13.1 and 13.2 put it there: 3a keeps `Channel`,
reached as `BuildContext::events()`, and 3c replaces it). Slice 5 builds over it (`Report<spec>`, control-plane.md) and
beside it (the realtime ring the framework makes per run). Reasons: every loop handler needs a generation-checked way
out; the carried log patch (section 9) needs a node log that carries `node` and `run`, which `LogEntry` already holds
(`log.hpp:17-25`) [V]; one name avoids a second rename in slice 5.

```cpp
// ear/core/run_handle.hpp: replaces InsidesEvents (node_type.hpp:171-180)
/// One run's way out, from any of its threads; never blocks. Shared-owned by the framework, so it outlives every thread
/// that can reach it, a leaked run's included. What it carries lands only on this run (I6). A reconfigure keeps it.
class RunHandle {
public:
	virtual const NodeName& node() const noexcept = 0;
	virtual std::uint64_t generation() const noexcept = 0;   // the `run` of its records
	/// A fact: the run cannot go on (framework page T10). The node fails `run-lost` with `detail` once the run has been
	/// `running`, and `start-failed` before that (in `built`, or from a consumer's `begin`); the framework page's 6.2 is
	/// the codes' home, and recovery's first wait follows the phase (its 8.6). During a reconfigure it fails the node
	/// (T10: the run itself is lost, not a replacement; T7 is for the token's `fail`). Ignored once close began.
	virtual void lost(std::string detail) = 0;
	/// Free text for people (framework page 8.8). The framework sets `node` and `run`; an empty `source` is the type's
	/// name. Straight to the log sink (any thread, never blocks), paced per node by the encoder as today. Dropped once the
	/// run is detached; a line racing the detach may land just after it, carrying the old `run`.
	virtual void log(LogEntry entry) = 0;
	void debug(std::string text) { log({.level = LogLevel::debug, .text = std::move(text)}); }
	void info(std::string text) { log({.level = LogLevel::info, .text = std::move(text)}); }
	void warn(std::string text) { log({.level = LogLevel::warn, .text = std::move(text)}); }
	void error(std::string text) { log({.level = LogLevel::error, .text = std::move(text)}); }
	/// Interim, until slice 5 splits notes into status and events: `node <n> says <text>`, on the main loop in order.
	/// Paced per run: 32 at once, then one a second; the next note that passes is preceded by `notes-dropped <n>`. A
	/// reconfigure does not reset it (the run goes on); a restart does. Heard until the run is detached, so a goodbye
	/// note is heard.
	virtual void say(std::string text) = 0;

protected:
	~RunHandle() = default;
};
```

- **Order.** `lost` and `say` cross to the main loop in order (as `Channel` does today, `node_host.cpp:173-226` [V]),
  so a note is ordered with node states. `log` goes straight to the sink and is not ordered with states, as captured
  output is not today. Its detach check is an atomic flag.
- **Pacing.** `say`'s pacing is sendspin's `NoteGate` (`note_gate.cpp:11-12`) moved into core, one gate per run;
  `lost` is never held back.
- **Where it is reached.** `bx.handle()` in `build` (G17), `rx.handle()` in `reconfigure`; the run object keeps the
  reference. `setup` gets none: it only acquires, so it has nothing to tell.
- **The sink.** `NodeHost::logTo(LogSink&)`, wired in `main.cpp` to `StatusLog`, the sink captured output already uses;
  `status_log.cpp` already writes `run` when present [V].
- **A leaked run's handle** is detached from the node and re-pointed at the leaked-run record (6.10), which hears its
  late `closed` calls; `lost`, `log` and `say` from it are dropped.

## 6. The primitives

### 6.1 Each primitive through the lifecycle

The framework page's states (6.2 there) against this page's primitives. "—" means the primitive is not touched in that
state.

| Primitive | `setting-up`: `setup` | `setting-up`: readiness (step) | `built`: `build` | `running`: `start`, then | `reconfiguring` | Teardown (I5 step) |
|---|---|---|---|---|---|---|
| Claim (6.9) | `sx.claim(...)`; implicit for stores, recordings, RTP receivers | judged first (1), then ports probed | — | held | `rx.claim` judged now; `rx.unclaim` released at the end; unchanged ones kept | released last (5); abandoned or kept by a leak (I5a) |
| Store (6.8) | declared in the spec: no call | opened on the I/O executor (2), 5 s | `bx.store(h).text()` | `replace`, `replaceLater` | kept | usable until 4b; freed (5) |
| Recording (6.11) | `sx.recording({path})` | claimed (1), opened (2), 5 s | `bx.recording(path)`: a descriptor | written by its consumer | `rx.recording(...)` replaces it make-before-break | closed (5) |
| Loop (6.4) | declared in the spec | started (3), nothing armed | `bx.loop(h)`; arming is staged | armed in `start`, released once it returned | kept; `rx.whenReady(loop, ...)` posts to it | closed to others (1); closed (4b); freed (5) |
| Run handle (5) | — | — | `bx.handle()` | `lost`, `log`, `say` | `rx.handle()`, the same | detached at 1 for facts; re-pointed at a leak |
| Source writer (6.5; a companion's) | `sx.source(bank, ...)` | the companion's (3') | `bx.source(bank)` | written by one thread | kept, or replaced by `rx.source` | closed (1); ring freed (5) |
| Sink consumer (6.6; a companion's) | `sx.sink(bank, ...)` | the companion's (3') | `bx.sink(bank)`; the consumer named in `Built::consume`; `begin` (4') | armed with the code's first drive (T4); `consume` on the sink's thread | `pause` and `resume`; or replaced by `rx.sink` | drained, `finish` (3); ring freed (5) |
| I/O executor (6.7) | — | runs the opens (2) | — | runs store writes | runs the replacement opens | completions after the run are dropped |

So a primitive bound to the run (a store, a loop, the handle) is made once per run and survives every reconfigure (R11);
a primitive bound to a setting (a claim on a port, a recording on a path, a companion) is replaceable in a reconfigure.

**This page's members of the facades.** The framework page's 6.10 declares every facade whole; these are the members
whose rules are this page's, with where:

| Facade | Member | Rules |
|---|---|---|
| `SetupOf<spec>` | `claim(claim::TcpListen)`, `claim(claim::Udp)` | judged at readiness step 1 (6.9) |
| `SetupOf<spec>` | `recording(RecordingRequest)` | claims `recording:<path>`; opened at step 2 (6.11) |
| `BuildOf<spec>` | `handle()` | the run handle (5; G17) |
| `BuildOf<spec>` | `store(handle)`, `loop(handle)` | a handle of `Spec.stores` or `Spec.loops` (static); open and trusted (6.8); running, arming staged until `start` (6.4) |
| `BuildOf<spec>` | `recording(path)` | its descriptor; `setup` asked for it (6.11) |
| `BuildOf<spec>` | `source(bank)`, `sink(bank)` | the writer (6.5) and the control (6.6) |
| `ReconfigureOf<spec, Run>` | `handle()`, `claim`, `unclaim`, `recording` | the run's handle, unchanged (5); claims make-before-break (6.9); recordings (6.11) |
| `ReconfigureOf<spec, Run>` | `whenReady(loop, work)` | the one later answer (6.2) |

- **Stores and loops are not asked for**: the spec declares them, so every run of the type has them, the boot sweep
  knows every store before any node exists, and slice 5's `writer::loop` and `call<..., Loop>` take a loop handle the
  spec lists (control-plane.md).
- **Look-ups that fail** at run time (`bx.recording(p)` for a path `setup` did not ask for, `bx.sink(b)` for a bank with
  no sink) throw `std::logic_error` naming the node and the handle: a type bug, found by its first harness test. Store
  and loop handles the spec lacks do not compile.
- **Every `sx.` call runs inside `setup` only**, every `bx.` call inside `build` (or the run object's constructor called
  from it), every `rx.` call inside `reconfigure`; else `std::logic_error`, as `StartContext` does today
  (`node_host.cpp:126-127`) [V].
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
  `RunLoop`, `RunStore` and `RunRecording` are the framework's sides of a loop, a store and a recording; a type sees
  only `Loop`, `Store` and a descriptor.

**Readiness**, this page's steps of the framework page's 6.4, ordered by cost and side effects so that a refusal costs
nothing:

| # | What | When it fails |
|---|---|---|
| 1 | **Claims**, in the same main-loop turn as `setup` returned: each is judged (6.9). A refusal fails the node now, and nothing below is touched. A wait holds every later step. Once all are granted, ports are probed (main thread, a non-blocking bind) | `start-failed: <claim's text>` |
| 2 | **Stores and recordings**, opened on the I/O executor, one job each, in request order (5 s each) | `start-failed: <file>: <why>`; `start-failed: cannot open <path>: <why>` |
| 3 | **Loops**, once step 2 is ready: dedicated threads started, strands registered. Threads last, so a refused store starts none | `start-failed: cannot start loop <name>: <errno text>` |
| 3' | Companions (the lifecycle's), once the block is ready and step 1 granted, in parallel with 2 and 3 | `engine-refused: <detail>` |
| 4 | `build` (the lifecycle's), once 3 and 3' are ready | `build`'s own `FailureReason` |
| **4'** | **Every sink consumer has begun**: `begin()` returned on its sink's thread (6.6), 5 s | `start-failed: <what begin passed to lost>`, or `start-failed: <bank>: consumer did not begin within 5 s` |

Then T4: once the block is ready, one data-loop invoke installs the code and arms the sinks (framework page I4), and
`start` runs once it completed. **A `setup` that fails** has
judged, opened and started nothing; what it asked for is dropped on the main loop. **A `build` that fails** tears the run
down by I5 (6.10): its loops run with nothing armed (staged sources are destroyed unrun with the run object, 6.4); 4a is
skipped; 4b closes them.

### 6.2 A reconfigure that goes on on a loop: `whenReady`

**Purpose.** A reconfigure often cannot act until its new acquisitions are ready (sendspin's new client must not start
before its port is granted and probed), and must act on the run's own thread, not the main loop (I11). `whenReady` joins
the two, so a type writes neither a poll nor a second hook.

```cpp
/// The reconfigure's token (framework page 6.6): move-only; done() or fail(detail) once, from any thread. Destroyed
/// without either, it fails the reconfigure ("<node>: the reconfigure was dropped"). After the run's close began, both
/// are ignored.
class ReconfigureDone {
public:
	void done() &&;
	void fail(std::string detail) &&;
};
/// In ReconfigureOf<Spec, Run> (framework page 6.10): once every acquisition this hook made is ready (claims granted and
/// probed, recordings open, replacement companions running and linked), posts `work(run, token)` to `loop`, a handle of
/// the spec's loops, as the run's own work. Returns the only "later" Reconfigured there is: the hook returns it. With
/// nothing acquired, posts at once.
template <class Loop>
Reconfigured whenReady(Loop loop, std::move_only_function<void(Run&, ReconfigureDone)> work);
```

- **If an acquisition is refused or lost**, or the reconfigure's deadline passes before the post, `work` is destroyed
  unrun on the main loop and the reconfigure falls back to a restart (T7). Its token's destructor then does nothing.
- **If a newer change supersedes the reconfigure** before the post (framework page T15), or the `restart` action ends
  it, `work` is destroyed unrun the same way and its acquisitions are released; the post is the commit point after
  which a newer change waits.
- **Once posted, `work` is the run's own work** (6.4's intake): it runs even if a stop or a restart began after the
  post, before the goodbye that 4a posts behind it, so a type's goodbye always sees a whole state, old or new, never
  half a swap. Its `done` is then ignored.
- **The deadline** (framework page 6.6, step 5: 10 s by default, a type may declare up to 60 s with `.lifecycle`,
  6.13) counts from the hook's call, claim waits included (6.9).
- **The code.** A continuation may not swap the code or hold banks: those are `rx` calls, made in the hook, applied by
  the framework at the end (framework page 6.6, step 4).
- **Harness.** `h.reconfigure(settings)` calls the hook and readies its acquisitions as `h.setup()` does; the
  continuation runs on the next `runLoops()` (section 10).
- **Why it is the only later answer.** Revision 3 as first written had a separate `rx.later()` token and a
  `Reconfigured::later()` result, two calls that had to agree. With `whenReady` alone a token and a later answer cannot
  disagree, and the type never has to learn when its claim was granted (a callback per kind, or a poll): `whenReady` is
  the reconfigure's `build`, its code run once everything it asked for is ready. A type whose library finishes on its
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
- **Rung before it is watched.** A source plays from T4, before `start` arms `onWake`, so a wake may be pending when its
  watch is armed; arming a watch on a pending wake runs the handler once, on the loop's first turn after the staged
  sources are released.
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

**Purpose.** A serial executor for a run's work off the realtime thread and the main loop (Q5: strands by default,
dedicated on request), declared in the spec (G14), alive for the whole run, reconfigures included.

```cpp
// ear/core/loop.hpp
namespace loop {
/// A loop of a type, declared in its spec. Name: 1 to 8 of [a-z0-9-], unique in the spec (both checked at compile time).
template <spec_detail::LoopName Name> inline constexpr LoopHandle<Name, LoopKind::strand> strand{};
template <spec_detail::LoopName Name> inline constexpr LoopHandle<Name, LoopKind::dedicated> dedicated{};
}
template <auto... Handles> struct Loops;   // TypeSpec's `.loops` (6.13)

/// A timer or a watch. Destroying it cancels it: its handler never runs after the destructor returns. Destroyed on its
/// loop's thread; or on the main loop while staged (in build, the run object's constructor or start); or on any thread
/// once its loop is closed (4b). Anywhere else is a type bug (asserted in debug builds and in the harness).
class LoopSource { public: virtual ~LoopSource() = default; };
class Loop : public Executor {
public:
	// Executor::post (executor.hpp): any thread; never runs `work` inside the call; serial, in post order. This is the
	// run's own intake: accepted until 4b. Work posted after 4b is destroyed unrun, on the caller's thread.
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
class RunLoop : public Loop {   // the framework's side
public:
	/// Main thread. Work from outside the run (slice 5's actions): posted while the loop is open to others, else
	/// destroyed unrun at once on the main thread (its reply object answers from its destructor).
	virtual void postFromOutside(std::move_only_function<void()> work) = 0;
	/// Main thread, at I5 step 1: refuses outside work from now on, and destroys queued outside work unrun on the loop's
	/// thread. The run's own work and sources go on; a handler already running finishes.
	virtual void closeToOthers() = 0;
	/// Main thread, at I5 step 4b: takes no more work, cancels every source still armed (counting them), destroys
	/// queued work unrun on its own thread, lets an in-flight handler finish, then calls `closed(leftSources)` on the main
	/// thread. Never joins on the main loop.
	virtual void close(std::move_only_function<void(std::size_t leftSources)> closed) = 0;
};
class LoopAdapter {  // ear_runtime: StrandPool and DedicatedLoops behind one adapter
public:
	/// Main thread. The loop runs (its thread started, or its strand registered) when `ready` is called, on the main
	/// thread, never inside make; `failed` with the reason otherwise (start-failed).
	virtual std::unique_ptr<RunLoop> make(const NodeName& node, std::string_view name, LoopKind kind,
	                                      ReadyEvents& events) = 0;
protected:
	~LoopAdapter() = default;
};
```

- **Staging, ended by `start`.** Everything sent to a loop from the main loop between readiness step 3 and the end of
  `start` (sources armed, work posted, in `build`, in the run object's constructor or in `start`) is staged on the main
  loop, not handed to the loop's thread. When `start` returns, the framework releases the staged items to their loops,
  in the order they were made. If the run is torn down before (a `build` that fails, a block lost while `built`), the
  staged items are destroyed unrun with the run object. So no handler runs against a run object that is not built, and
  no handler runs before the code drives the block (framework page 6.4: "nothing of the run object's runs before
  `start`"). The convention: `build` constructs and arms nothing; `start` arms (4.1).
- **In a reconfigure** a loop is the run's, unchanged. The hook (on the main loop) may post to it; such work is staged
  until the hook returns, then released, so a hook never races its own posts. `whenReady`'s continuation is posted later
  (6.2).
- **The source contract.** Before a run object calls `closed`, it has destroyed every source it armed (on the source's
  loop, as sendspin's `close` does in 4.1). The framework counts live sources per loop. At 4b a source still armed is a
  type bug: the loop cancels it itself (so its handler never runs again), the harness fails the test, debug builds
  assert, and a release build says `node <n> ear.loop-source-left: <k> sources were still armed on loop <name> when the
  run closed` and goes on.
- **Two intakes.** From step 1 a loop refuses work from outside the run (`closeToOthers`): slice 5's actions use
  `postFromOutside`, so a queued call is answered `restarted` or `stopping` at step 1, and one already running finishes
  and replies before the goodbye that 4a posts behind it. The run's own posts and sources (the goodbye, a posted
  reconfigure continuation) are accepted until 4b.
- **Threads.** A handler runs on the loop's thread, one at a time per loop. A strand runs on a framework pool thread
  (`min(4, hardware threads)` to start, G9); a dedicated loop has its own thread, named `ear-<name> <node>`, cut to 15
  bytes. Nothing here is realtime. A dedicated loop may block (a library's poll, a disk write, a source write with
  patience); a strand must not: debug builds assert at every blocking primitive (`SourceWriter::write` and `idle`,
  `Store::replace`) that the caller is not a strand.
- **A slow strand.** A strand handler running past 100 ms is `node <n> ear.loop-slow: loop <name> ran one handler for
  <ms> ms`. Past 1 s the pool starts a replacement thread (at most 16 in all), so one stuck strand delays no other node
  (R16); the stuck thread rejoins the pool when its handler returns. A dedicated loop is never judged.
- **Lifetime.** Started at readiness step 3; closed to others at step 1; closed at 4b, after the run object's `closed`,
  so its goodbye may still post to its loops; freed at step 5, after the run object (their handlers point into it). A
  dedicated loop's thread is joined at step 5, after it posted that it exited, so the join returns at once. A loop of a
  run that never got as far as step 3 is never made.
- **What a loop is not.** No priority, no affinity, no realtime scheduling; no futures; no sharing across runs. A type
  that wants a 10 ms poll asks for it (`every(10ms)`).
- **Harness.** Manual: each loop is a queue the test turns, on a fake clock (section 10).
- **Replaces.** `SendspinWorker`'s two `std::thread`s (`worker.cpp:93`, `200`), its stop `EventFd`, `waitForStop`, the
  10 ms `poll()` loop (`worker.cpp:278-299`) and the notifier's (`309-323`) [V]. `ConnectionWatch` stays the type's: a
  pure judge the tick feeds.

### 6.5 A source's writer: one at a time, and its closing contract

The source companion is the framework page's (9.6: its ring, its realtime side, keeping time, played reports). This is
the home of what a type's thread sees when it writes one.

```cpp
class SourceWriter {   // bx.source(bank)
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
  `src/sync_task.cpp:56-58`) [V] sleeps between calls from step 1 until 4a stops it, and never spins. The cost: a type's
  own patience bounds how long its writer can delay its goodbye after the close (sendspin's: 20 ms).
- **A replaced source** (`rx.source`, make-before-break, framework page 6.6) has its own writer; the old writer is
  closed at the reconfigure's end, by the same contract.
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
class SinkConsumer {                                   // a member of the run object, named in Built::consume
public:
	/// The sink's thread, once, before any consume and before the node is `running` (readiness step 4'). Open what the
	/// consumer writes to (file's WAV, a socket). A failure is handle.lost(detail): the node fails from `built`.
	virtual void begin() {}
	virtual void consume(const Chunk& chunk) = 0;      // in order; may block on I/O; framework page 9.6
	virtual void finish() = 0;                         // once, after the last consume (I5 step 3)
protected:
	~SinkConsumer() = default;
};
class SinkControl {                                    // bx.sink(bank): the run object keeps it
public:
	/// Any of the run's loops. The sink's thread finishes a consume in flight, then discards chunks (counted as paused
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
  `start`, `file.cpp:76-79` [V]). `begin` runs on the sink's thread once `build` returned and `Built::consume` named the
  consumer, so T4 waits for it (step 4'), and the WAV is open before the first chunk. No frame reaches the ring before
  the sink is armed, in the same invoke as the code's first drive at T4, so nothing is discarded (revision 2's visible
  change 4 is gone).
- **Why `pause`.** A consumer that writes into something its run swaps in place (sendspin's library client, 4.2; the RTP
  sender's socket, 4.3) needs a moment when no `consume` runs. Without it every such type writes a lock around its
  target on the sink's thread; with it the framework owns the one hand-over, and the type's swap reads as a sequence
  (G21). Paused audio is discarded, not queued: a swap is a break in what the node sends, and the chunks' positions say
  where.
- **Teardown.** The sink is disarmed in the barrier's own invoke at step 1 (framework page I4), so the ring holds
  exactly the cycles the code drove, and step 3 drains them, none of the silent block's zeros after them. A paused sink
  at step 3 is drained by discarding, then `finish` is called; a consumer's `finish` must cope with a pause it never saw
  resumed. A replaced sink (`rx.sink`) drains to its own consumer and finishes at the
  reconfigure's end; its replacement's `begin` runs as it becomes ready, before the reconfigure ends.
- **Counters.** `pausedFrames` beside the framework's drops; slice 5 samples both (control-plane.md).
- **Harness.** The framework page's fake sink hands chunks straight to the consumer on the test's thread; `begin` runs at
  `h.build()`; `pause` posts `paused` to the named loop at once; `h.paused(bank)` and `h.pausedFrames(bank)`.
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

- Types never see it. In slice 3c it opens stores and recordings (readiness step 2 and a reconfigure's replacements) and
  runs every store write; slice 8's `/proc` reads and slice 9's runtime files will use it.
- **Jobs hold shared state.** Every job holds a `shared_ptr` to the state it fills (the store's text and mode, the
  recording's descriptor), never a `RunStore`, `RunRecording` or `ReadyEvents` pointer. Its completion is posted to the
  main loop and checked there against the run's generation and the object's liveness: a completion for a run that is
  gone, for a reconfigure that ended, or past its deadline, is dropped, and a descriptor it opened is closed by a second
  I/O job. So a run torn down while its open is in flight frees nothing a job will touch.
- **One thread**: a hung disk stalls every open and every store write (section 16). Each has a 5 s deadline, so a node
  fails visibly instead of starting forever (G13).
- **Stopping.** `main.cpp` asks it to stop after `NodeHost::stopAll` completed and the keeper's thread was joined:
  queued jobs are destroyed unrun, and the job in flight gets 2 s. If it is still busy then, ear writes a `log` record
  (`source` `ear`, `warn`: `the I/O thread was still busy at exit: <job>`) and ends with `std::_Exit(0)` once the output
  queue is written out, as it does for a leaked run (`main.cpp:312-315`) [V]. `ear.conf` is written by then.

### 6.8 Stores

**Purpose.** A file that belongs to one node and survives its runs: named by the framework, kept narrow, read and
trusted before the run is built, replaced only if unchanged, swept at boot. A store is the run's for its whole life, so
an in-place reconfigure keeps it (sendspin's identity survives a port change, 4.2).

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

**The run's side:**

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
	/// The same without waiting, from any of the run's loops; `done` runs on `loop` with the result, or never once the
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
	virtual std::unique_ptr<RunStore> open(const NodeName& node, std::string_view type, const StoreDeclaration& store,
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
- **Open** at readiness step 2, after the store's claim was granted (6.9), never in `setup` or `build`, so no run object
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
  (I7) already makes a store one run's; the claim covers the leaked run that I7 does not.
- **Lifetime.** Opened at readiness (5 s: `<file>: not opened within 5 s`); usable until 4b; kept through every
  reconfigure; freed at step 5, after the loops (a write in flight holds the store's shared state, and its `done` is
  dropped).
- **Harness.** In memory, by handle (section 10).
- **Replaces.** `NodeKeyStore::open` (`key_store.cpp:64-97`), `narrowKeyFiles` (`51-62`), its call and boot log
  (`sendspin_type.cpp:137-144`), and `NodeFolders` entirely [V]. `NodeKeyStore` keeps only the library's provider
  interface (`load_blob`, `save_blob`) over the parsed record, writing with `Store::replace` from the worker or a library
  thread.

### 6.9 Claims

**Purpose.** Something ear-wide that one run at a time may hold (I8): a listening TCP port, a UDP port, and, implicitly,
a store's file and a recording's path. A claim is bound to a setting, so a reconfigure may swap it make-before-break.

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
| `tcp:<p>` | `sx.claim(claim::TcpListen{p})`, `rx.claim(...)` (sendspin listening) |
| `udp:<p>` | `sx.claim(claim::Udp{p})`, and implicitly every RTP receiver companion (`rtp`) |
| `store:<file>` | implicitly, every store of the spec, for every run |
| `recording:<path>` | implicitly, every `sx.recording(...)` and `rx.recording(...)` |

**Judged at readiness step 1**, or when a reconfigure's hook returns, against the key's holder:

| Holder | Outcome | Detail (when it fails) |
|---|---|---|
| none | granted, then probed (ports only) | the kernel's reason |
| the same run (a reconfigure asking a key its run holds) | its own: granted, not probed (G22) | — |
| a run that is not going: a node running, setting up, built or reconfiguring | refused at once | `held by node <n>` |
| a run that is **going**: any run between I5's step 1 and step 5, of any node (a restart's, a failure's, a removal's or a rename's old run) | waits: `ear.claim-wait` says why; granted when that run's step 5 releases it, then probed | after 15 s (in a reconfigure, its deadline): `held by node <n>, which did not let go within 15 s` |
| a leaked run (I5a), port | abandoned: probed afresh; granted if it passes | the kernel's reason, and the node starts by itself at the late close (R15) |
| a leaked run, store or recording | refused | `held by a run of node <n> that did not close; it starts when that run closes` (R15) |

- **The wait, visible.** When a wait begins, `node <b> ear.claim-wait: <key> held by node <a>, which is closing` (a
  `diagnostic` record, in `output-records.json` from S3c.5). The holder's own close deadline (10 s) passes before the
  waiter's (15 s), so a wait ends in a grant, a port's abandonment, or a refusal that names the leaked run.
- **A node refused by a leaked run starts by itself when that run closes late** (R15). The leaked run's late close
  (6.10) releases its claims, then starts again, once, every node still `failed` by a refusal that named it, as the
  `restart` action would (framework page T8). A port whose fresh probe failed while a leaked run held it is treated the
  same.
- **In a reconfigure** (framework page 6.6, step 3): `rx.claim` is judged when the hook returns, before any other new
  acquisition; a refusal ends the reconfigure (T7) and destroys its continuation unrun. `rx.unclaim` names a key the run
  holds (else `std::logic_error`) and releases it at the reconfigure's end, after the replaced resources, last. So a
  node moving from 8928 to 8929 holds both for a moment, and an operator's second node can take 8928 only once the
  first is `running` on 8929. A claim wait inside a reconfigure counts against the reconfigure's deadline (10 s by
  default), not its own 15 s; past it the fallback restart's `setup` waits afresh. Two nodes swapping ports in one edit
  each find the other's claim held by a running run, so both fall back to restarts; each restart's claim then finds the
  other's old run going, and waits for its step 5, whichever teardown finishes first (framework page 6.6, step 7, and
  I8). A node never waits on its own old run: its `setup` comes after that run's step 5 (I7).
- **A replacement on the same key.** An RTP receiver replaced with its port unchanged (`rtp` changing `format` on 5004)
  asks `udp:5004`, which its run holds: its own. Two rtp-source modules bound to one port with SO_REUSEADDR would split
  or duplicate the datagrams [U13], so the framework replaces such a companion break-first without the type asking: it
  holds the public banks the receiver feeds through the pass-through, waits for the hold's acknowledgement (the fade
  reached zero, framework page 6.6), destroys the old receiver, then makes the new one; so the gap is a fade, silence
  and a fade in, whatever the type held. A sendspin port change never asks
  the same key (4.2 claims only when the port changes).
- **Release.** At step 5, last, or at a reconfigure's end for an `unclaim`. A waiter is granted in arrival order. A
  waiter of a run torn down, or of a reconfigure ended, before its claims were granted leaves the queue at once.
- **The probe's limit stays:** a program taking the port just after the probe is not caught; sendspin's library retries
  every 5 s (`sendspin.md` section 10), and an `rtp` receiver logs PipeWire's bind failure as today.
- **mDNS names are not claimed.** sendspin's instance name is `ear-<node>-<first 8 of client id>` (AGENTS.md), and the
  client id comes from the node's own key file, so two nodes cannot advertise one name.
- **Harness.** Granted by default; `h.holdClaim(claim::TcpListen{8928}, "porch")` refuses one, `h.waitClaim(...)` makes
  one wait and `h.releaseClaim(...)` ends the wait, `h.failProbe(...)` makes a probe fail; `h.claims()` lists what the
  run holds, both ends of a reconfigure included.
- **Replaces.** `PortClaims`, `HeldPort`, `probeListenPort` (`port_claims.*`, `sendspin_type.cpp:36-47`, `83-92`) [V],
  and their type-level scope: a claim now spans types (an `airplay` node on 7000 against anything else on 7000; an `rtp`
  receiver against an `rtpsend` sender on one port).

### 6.10 Teardown, full and partial

I5 (framework page 6.9) with every primitive of this page. Every step that waits on another thread is asynchronous; the
main loop never joins a thread that has not already said it is done.

| Step | What | Threads it waits on |
|---|---|---|
| 1 | The fade and its acknowledgement, then the barrier (`drive(nullptr)`, or the block destroyed), which disarms every sink in the same invoke; `release`, holds cleared; every source writer closed (6.5); every loop closed to others (queued actions answered); a reconfigure in flight ended (its pending acquisitions dropped, its continuation destroyed if not yet posted, its token ignored); if the run is not ready yet: its claim waits leave the queue, its pending opens are orphaned (6.7), and the readiness steps it had not reached never start | the data loop, asynchronously: the step continues from the invoke's completion, posted back (framework page I4) |
| 2 | Companions destroyed (the lifecycle's) | none |
| 3 | Sinks drained: each hands what its ring holds to its consumer (a paused sink discards it) and calls `finish` on its own thread, then reports | sinks' threads |
| 4a | The run object's `close(closed)`, if `build` made one: its goodbye may post to its loops; it stops threads its library owns; it destroys every source it armed; then `closed` from any thread | the run object's loops and threads |
| 4b | Each loop closed: no new work, sources still armed cancelled and counted (`ear.loop-source-left`), queued work destroyed unrun on the loop's thread, an in-flight handler finishes; each reports | loop threads |
| 5 | On the main loop: the code; the run object; the companions' rings and wakes; loops (a dedicated thread joined, already exited); stores; recordings closed; claims released (waiters granted); the config | none |

**A run torn down before it was ready** (a change, a removal or a stop while it waits on a claim, an open or a
companion): step 1 as above; steps 2 and 3 for what exists; 4a is skipped (no run object); 4b closes whatever loops
readiness step 3 had started, with nothing armed; step 5 frees what exists. **A run whose `build` failed** is the same,
its staged sources destroyed unrun with nothing to free.

**A reconfigure's end** (framework page T6) releases what it replaced by the same steps, for those resources only, after
its commit point's one invoke (the new code and layout installed, replaced sinks disarmed and their replacements
armed): a replaced source's writer closed (1); replaced companions destroyed (2); replaced sinks
drained to their own consumers and finished (3); no 4a or 4b (the run object and its loops go on); replaced recordings
closed, then `unclaim`ed claims released, last (5). **A reconfigure that is superseded** (framework page T15) releases
its uncommitted replacements by the same partial steps, and the run keeps everything it had. **A reconfigure that
falls back** (T7) tears the whole run down, old and new resources alike, by the full table.

**The close deadline and the leaked run** (I5a; R15). The deadline (default 10 s) covers steps 3, 4a and 4b together,
as slice 1 built for 3 and 4. A run past it is leaked: everything step 5 would free is kept, an `ear.close-overdue`
diagnostic names what was still open (`<n> sinks open, run object closing, <k> loops open`), its port claims are
abandoned and its store and recording claims stay (6.9), and the node moves on. Unlike slice 1 (`node_host.cpp:446-464`,
where the detached `Channel` drops every later report, `191-221`) [V], the leaked run keeps listening:

- Its handle is re-pointed at a **leaked-run record** that `NodeHost` keeps (in a list, never freed). Every later sink
  report and run-object `closed` lands there, in order on the main loop.
- The record carries on where the run stopped: a run leaked in step 3 is told to close (4a) once its last sink reports,
  then its loops are closed (4b); a run leaked in 4a has its loops closed once `closed` comes.
- When everything it held has reported, its claims are released (waiters granted), `node <n> ear.close-late: the run
  leaked at the close deadline closed after <s> s; its claims are released` is said, and every node its claims refused
  is started again (6.9, R15). Its memory stays leaked: the rule "a leaked run is never freed" stays simple.
- `NodeHost::leakedRuns()` counts leaked runs still open; one that closed late no longer counts, so a stop after it ends
  normally rather than with `std::_Exit` (`main.cpp:312-315`) [V].
- A run whose threads never report stays leaked and open until ear exits, and the nodes it refused stay `failed` with a
  detail that says why.

### 6.11 Recordings, deferred

**Purpose.** A file beneath the recordings folder that one run writes (slice 2's kind, `node_type.hpp:164-167` [V]), now
opened after its claim and off the main loop, written by a sink's consumer.

```cpp
struct RecordingRequest { std::string path; };   // unchanged: relative to the recordings folder
// setup:        sx.recording({path});            claims recording:<normalised path>; opens nothing
// build:        int fd = bx.recording(path);     the descriptor, open, created or truncated
// reconfigure:  rx.recording({newPath});          claimed and opened beside the old; the old closed at the end
class RecordingAdapter {   // node_host.hpp, amended; main.cpp's ConfinedRecordings over ear_folder
public:
	/// Main thread. Creates or truncates `path` beneath the folder on the I/O executor (openat2, RESOLVE_BENEATH, as
	/// today); `events.ready()` with the descriptor in the run's shared state, or `events.failed(<why>)`.
	virtual std::unique_ptr<RunRecording> open(const NodeName& node, const std::string& path, ReadyEvents& events) = 0;
protected:
	~RecordingAdapter() = default;
};
```

- **Order.** Asked for in `setup`, judged as a claim at step 1, opened at step 2, reached in `build`, the WAV header
  written in the sink consumer's `begin` (6.6), before `running`. So a path held by a running node is refused before
  anything is truncated, and a path held by a node still closing waits (I8): removing `rec` and adding `rec2` on
  `take1.wav` in one plan works.
- **Failure text.** The framework writes `cannot open <path>: <why>`, today's text (`file.cpp:71`, `79`) [V], so the system
  tests keep their lines; a claim refusal is `cannot open <path>: held by node <n>`; libsndfile's refusal in `begin`
  is `cannot open <path>: <libsndfile's reason>`, before `running`, as today.
- **`file` on it.** `setup` asks for the recording and a sink on its inside bank `writer` (framework page 9.6); `build`
  makes `FileRun` around the descriptor and hands its `WavWriter` to the sink; `begin` calls `sf_open_fd`; `consume`
  writes; `finish` closes the WAV. `path` stays restart-only (framework page 6.6); `rx.recording` exists for a later
  gapless switch.
- **Lifetime.** Opened at readiness (5 s); closed at step 5, after stores and before claims are released, so the next
  run's open of that path follows the close; leaked with a leaked run, its claim kept (6.9).
- **Harness.** A real descriptor on a temporary file, or a failure the test sets (section 10).
- **Replaces.** The synchronous open in `acquireResource` (`node_host.cpp:126-141`) and `file`'s main-loop
  `sf_open_fd` (`file.cpp:76`) [V].

### 6.12 What a type author no longer meets

For the reader of revision 2: `StartOf<spec>` is now `SetupOf<spec>` (it returns no code), `ReadyOf<spec>` is now
`BuildOf<spec>` (it is handed over once every resource is ready, and the run object it makes owns its state), and the
recipe is gone for spec types (G15). A spec type's `setup` that returns a code, or a `build` of the wrong signature, does
not compile (the lifecycle's compile-fail cases); hand-made `NodeTypeOf` types are test-only after S3c.6.

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
	Lifecycle lifecycle{};  // slice 3d: the reconfigure deadline, 1 s to 60 s (framework page 6.10)
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
| `setup` | the main loop | acquire: claims, recordings, companions. No I/O, no threads, no blocking |
| `build` and the run object's constructor | the main loop | read `Store::text()`, keep the recording's descriptor (not write it), take loops, the handle, writers and sink controls; hand over consumers. Arm nothing by convention (staged if it does). No I/O, no threads, no blocking |
| The run object's `start` | the main loop | arm loop sources and post (staged until it returns). No blocking |
| `reconfigure` | the main loop | the `rx` calls; post to the run's loops (staged until it returns); `whenReady`. No blocking |
| `prepare`, `release` | the main loop, never with `process` | allocate |
| `process` | the realtime thread | `io` only (framework page I12) |
| Strand handlers | a pool thread, serially per strand | anything that does not block: `post`, `replaceLater`, `RunHandle`, non-blocking sockets, `SinkControl` |
| Dedicated loop handlers | the loop's thread | also block: `SourceWriter::write` and `idle`, `Store::replace`, a library's `start`, `stop` and poll |
| Threads a library owns (sendspin-cpp's) | their own | what the library does, `SourceWriter::write` and `idle`, `Store::replace`; the run object stops them in `close` |
| `SinkConsumer::begin`, `consume`, `finish` | the sink's thread | block on I/O; `begin` opens what the consumer writes to |
| A `whenReady` continuation, a `SinkControl::pause` callback | the loop named | as that loop's handlers; while paused, the consumer's state |

A guard joins `standard_output_guard.cmake` as its own script, `thread_guard.cmake`: no non-test file under
`src/nodes/` (it skips every path with a `/tests/` component, since sendspin's tests use `std::thread`,
`code_test.cpp:272`, `feed_test.cpp:80` [V]) names `std::thread`, `std::jthread`, `std::async`, `pthread_create` or
`fork`. Its own test covers each spelling, a look-alike it lets through (`my_thread_count`), and the `/tests/` exclusion
(G12). A type gets threads only as loops, and a library's own threads stay inside the library.

## 8. Invariants and failure modes

Invariants this page adds to the framework page's I1 to I13, each a test's to prove:

- **P1. No type code starts a thread, and no type code does I/O on the main loop.** Loops are the framework's; stores
  and recordings open on the I/O executor; `build` gets them open; a consumer opens what it writes in `begin`, on the
  sink's thread.
- **P2. No handler of a run runs before its `start` returned or after its run object called `closed`**, and every loop
  of a run is closed before its run object is freed (staging, the source contract, 4b).
- **P3. A claim has one holder ear-wide**, across types, judged before anything of the run is opened or started; a run
  in a reconfigure may hold an old and a new key, never two runs one key; a leaked run's claim on something no probe can
  check is never handed on before that run's late close.
- **P4. A store's file is never wider than its mode** after boot, an open or a write, and never replaced by a new
  identity when its text is distrusted.
- **P5. What a run keeps, a reconfigure keeps.** Loops, stores, the run handle and claims a reconfigure does not
  `unclaim` are the same objects before and after; a reconfigure's new acquisitions are ready before its continuation
  runs, and its replaced ones are released only after its end.
- **P6. One writer of a source and one consumer call of a sink at a time.** A source's writer changes only by a join or
  a post; a sink's consumer state changes hands only through `pause`.

| Failure | What the operator sees | Why it is safe |
|---|---|---|
| A store cannot open: a symlink, a directory, more than `maxBytes`, unreadable, distrusted, cannot be created | `node <n> starting`, then `failed start-failed: <file>: <why>`; the file unchanged | no run object built, no loop started |
| A store or recording open hangs (a hung disk) | after 5 s, `failed start-failed: <file>: not opened within 5 s` (a recording: `cannot open <path>: not opened within 5 s`) | the job holds only shared state; its late completion is dropped (6.7) |
| A later store write is refused | sendspin: `says key-write-failed <detail>` (today's note, its text from the store) | the file keeps the other writer's text; this run's state stays in memory |
| A claim held by another running node | `failed start-failed: cannot listen on port <p>: held by node <n>` (today's text, `sendspin_connections_test.cpp:235`) [V]; `cannot open take1.wav: held by node <n>` for a path | refused at step 1: no file opened, no thread started |
| A claim held by a node still closing | `starting`, `ear.claim-wait: <key> held by node <n>, which is closing`, then `running`; after 15 s, failed with `which did not let go within 15 s` | I8; the holder's close deadline (10 s) passes first |
| A port another program holds | `failed start-failed: cannot listen on port <p>: Address already in use` (today's text, `sendspin_connections_test.cpp:202`) [V]; for `rtp`, `cannot bind UDP port <p>: Address already in use` | the probe |
| A claim held by a leaked run | port: probed afresh; store or path: `held by a run of node <n> that did not close; it starts when that run closes`; later `ear.close-late` and the node running by itself | R15 |
| A reconfigure's claim refused | `reconfiguring: port`, `restarting: claim refused`, then `failed start-failed: cannot listen on port <p>: held by node <n>` | refused before the continuation runs: the old client was never stopped by it |
| A reconfigure's claim waits past the deadline | `reconfiguring`, `ear.claim-wait`, `restarting: reconfigure took more than 10 s`, the restart's own wait | the reconfigure's deadline (framework page 6.6) |
| A strand handler that never returns | `ear.loop-slow` after 100 ms; a replacement pool thread after 1 s; at teardown the close deadline leaks the run | other nodes' strands keep their threads (R16) |
| A dedicated loop that never drains | at teardown, after 10 s, `ear.close-overdue ... 1 loops open`; the node moves on | its memory and thread are leaked, never freed |
| A loop's thread cannot be started | `failed start-failed: cannot start loop <name>: <errno text>` | no run object was built |
| A run object that leaves a source armed | `ear.loop-source-left` (release build); the harness and debug builds fail | the loop cancels it at 4b |
| A source writer blocked at teardown | returns 0 at step 1; later calls sleep their patience | its wait is bounded by its own patience |
| A consumer's `begin` fails (libsndfile refuses the descriptor) | `starting`, then `failed start-failed: cannot open take1.wav: <libsndfile's reason>` | `lost` from the sink's thread, crossing to the main loop; the node never ran |
| A consumer's `begin` never returns | after 5 s, `failed start-failed: writer: consumer did not begin within 5 s`; at teardown the close deadline covers the sink's thread | readiness step 4' has its deadline |
| A post, ring or handle call after the run closed | dropped | loops destroy late work unrun; the handle is detached |
| The close deadline at stop | ear proceeds, as today; `main.cpp` ends with `std::_Exit` once any leaked run is still open | unchanged, but a run that closed late no longer forces it |
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

**ear's side**, in sendspin's run object, on the worker: `set_log_sink(&forward)` once per process (`std::call_once`),
and `config.log_context = &handle_` (the run's `RunHandle`). `forward` maps `SS_LOG_ERROR`, `WARN`, `INFO` and `DEBUG`/
`VERBOSE` to ear's four levels and calls `handle.log({.level, .source = "sendspin-cpp", .text, .topic = tag, .cut})`.
A null context becomes an unattributed `log` record from `sendspin-cpp`. Records then read:

```json
{"v":1,"seq":91,"t":"…","kind":"log","node":"kitchen","run":4,"source":"sendspin-cpp","topic":"sendspin.conn","level":"warn","text":"…","msg":"node kitchen sendspin-cpp sendspin.conn warn: …"}
```

- **Across a reconfigure.** The client 4.2 makes anew gets the same context, the run handle, which a reconfigure keeps,
  so its lines carry the same `run` as before the swap.
- **Lifetime.** The context is the run handle, which outlives the run (5). Each client is destroyed on the worker before
  `closed` (or before the swap's new one starts), so no thread of an old client logs after it is gone.
- **Cost.** A rebase at each bump of the pin, as for the listener patch; configure refuses a tree without it
  (`carried_patch_missing`, `cmake/CarriedPatch.cmake`) [V]. `sendspin_patched_dep` takes a list of patches.
- **Not upstreamed here.** Offering it upstream is worth a try (U8); ear keeps it carried until a release has it.

## 10. The type harness, for slice 3c

`TypeHarness<spec, Behaviour>` steps the lifecycle (framework page 7.6: `parse`, `setup`, `ready`, `build`, `start`,
`process`, `reconfigure`, `stop`) and fakes companions; this page adds the fakes of its primitives, each named by the
spec's handle (a misnamed or undeclared handle does not compile, as `harness.input(card::play, 0)` does not today,
`type_harness.hpp:23-30` [V]). `NodeHarness` under it takes names, for hand-made types.

| Kind | The fake | What a test does |
|---|---|---|
| Loop | a queue and a fake clock per loop; kind kept, so blocking asserts still judge strands | `runLoops()`; `advance(ms)`; `loop(worker)` to turn one; `armed(worker)` counts its live sources |
| Store | in memory, by handle | `presetStore(keyFile, text)`, `failStoreOpen(keyFile, why)`, `failNextWrite(keyFile, why)`, `storeText(keyFile)` |
| Recording | a real descriptor on a temporary file, or a failure | `failRecording(path, why)`, `recordingText(path)` |
| Claim | granted | `holdClaim(claim::TcpListen{8928}, "porch")`, `waitClaim(...)`, `releaseClaim(...)`, `failProbe(...)`; `claims()` |
| Readable descriptor | delivered by the test | `readable(fd)`: the `onReadable` handler runs on its loop's next turn |
| Run handle | captured | `notes()` (paced as the framework paces), `logs()`, the fact |
| Sink control | on the fake sink (framework page 7.6) | `paused(bank)`, `pausedFrames(bank)`; `begin` runs at `build()` |
| Source writer | on the fake source; never waits | `writerThreads(bank)` in debug builds, for P6 |

**Rules a test can rely on:**

- **Readiness.** `setup()` judges claims and opens stores and recordings at once, then makes the loops, unless the test
  made a claim wait or an open fail first; after `releaseClaim`, readiness carries on at once. `build()` calls `build`
  and every consumer's `begin`. `start()` calls the run object's `start`, then releases the staged sources.
- **The clock.** `runLoops()` runs posted work, rung wakes and readable deliveries on every loop until all are idle; it
  never fires a timer. `advance(d)` moves the fake clock by `d` and fires, in time order, every timer due at or before
  the new now, `after(0)` included, running each loop until idle after each.
- **Reconfigure.** `reconfigure(settings)` calls the hook; claims it asked for are judged as at setup; a `whenReady`
  continuation runs on the next `runLoops()`; `state()` stays `reconfiguring` until its token's `done` and every
  replacement is ready, then the replaced claims and recordings are released and `state()` is `running`.
- **Store writes.** A `replace` completes inside the call; a `replaceLater` completes, and its `done` runs, on the next
  `runLoops()`.
- **`stop()`** runs I5 as the framework does: release, writers closed, loops closed to others, sinks finished, the run
  object's `close`, then the harness turns loops until `closed` is called. **If every loop is idle and `closed` has not
  come, it fails at once** with `std::runtime_error` naming the loops and the sources still armed on each. Then 4b (a
  source still armed fails the test) and step 5. A real-time bound (10 s) applies only when the test chose to start real
  library threads.
- So a run object that starts its library from a timer (sendspin's `after(0)` in `start`) never starts it in a test that
  does not advance the clock: the all-type tests run sendspin without a network.

`notYetInHarness` and its checks in `test/nodes/harness_test.cpp` go in S3c.6 (framework page 13.1).

## 11. Where the code lives

| Package | Gains | Never |
|---|---|---|
| `ear_core` | `run_handle.hpp`, `loop.hpp`, the wake, store, recording, claim and probe interfaces, `IoExecutor`, `SinkControl`, `ReconfigureDone`; `ClaimRegistry` and the leaked-run record in `NodeHost`; the facades' members of 6.1, `whenReady`, `StoreRule`, `Stores`, `store::file`, `Loops`, `loop::strand`, `loop::dedicated` and `TypeSpec`'s new members in `type_spec.hpp`; `NodeType::stores()` | threads, files, sockets (unchanged) |
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
| `sendspin_type.cpp` (146) | renamed `sendspin.cpp`: `setup`, `build`, `reconfigure`, `makeSendspinNodeType` (about 50 lines). `SendspinConfig`, `SendspinType`, `HeldPort` and the boot sweep go |
| new `sendspin_spec.hpp` | rows (moved from `settings.hpp`, marked `.inPlace()` in 3d), banks, `keyFile`, `worker`, `notifier`, `spec`, `Sendspin`, `feedFramesFor`, `emptyKeyRecord`, `whyNotKeyRecord` |
| `settings.hpp`, `settings.cpp` (44, 67) | `ServerUrl`, `parseName`, `SendspinSettings`, `settingsFrom`; namespace only |
| `worker.hpp`, `worker.cpp` (37, 398) | become `run.hpp`, `run.cpp`: `SendspinRun : RunObject` built from `BuildOf<spec>&`, with the library's listeners (unchanged bodies but `on_audio_write`'s idle branch), `start`, `startClient`, `tick`, `notify`, `stopClient`, `close`, and in 3d `redial` and `swapClient`. Gone: both `std::thread`s, the stop `EventFd`, `waitForStop`, `poll()`, `finish`'s joins, `NoteGate`, `clientMutex_` (the sink's pause replaces it for `write_audio`; `clientTimeOf`, read only by `clock_test.cpp:434` [V], posts to the worker and waits for the answer). About 320 lines |
| `key_store.hpp`, `.cpp` (41, 141) | `NodeKeyStore` keeps `load_blob`, `save_blob` over a parsed `KeyRecord` and a `Store&`; `whyMalformed` becomes `whyNotKeyRecord`; `KeyRecord::parseTrusted`; `emptyKeyRecord`. `open` and `narrowKeyFiles` go. About 70 lines |
| `event_fd.hpp` (33) | deleted: `EventWake` and the source's room signal own their eventfds |
| `note_gate.hpp`, `.cpp` (27, 40) | deleted: the run handle paces `say` |
| `port_claims.hpp`, `.cpp` (24, 36) | deleted: `ClaimRegistry` (core) and `KernelPortProbe` (`ear_runtime`) |
| `to_server.*` | `ToServer : SinkConsumer` over stamped chunks (3b), holding the run's `SinkControl&` (3d) |
| `connection_watch.*`, `advertiser.*`, `pcm.hpp`, `peer_text.hpp`, `notes.hpp` | namespace only. `notes::notesDropped` goes (the framework says it). The advertiser's goodbye is called by `swapClient` too |
| `CMakeLists.txt` | the sources above; tests link the harness; `clock_test` links `ear_runtime` |

`file` (after 3b, it has a sink on `writer` and no realtime code): `setup` adds `sx.recording({path})`; `build` makes
`FileRun` with `bx.recording(path)` and `bx.handle()` and hands its `WavWriter` over; `WavWriter::begin` calls
`sf_open_fd` and reports a refusal with `handle.lost("cannot open <path>: <libsndfile's reason>")`. Its tests keep their
cases, now through the harness's recording fake, with the header written at `h.build()`.

sendspin's tests:

| Test file | Fate |
|---|---|
| `port_claims_test.cpp` (3) | the claim case moves to core (`ClaimRegistry`); the two probe cases (a live listener, `TIME_WAIT`) to `ear_runtime`, plus a UDP port held with SO_REUSEADDR refused |
| `notes_test.cpp` (6) | `NoteGate`'s cases move to core's run handle tests; `printable` and the texts stay |
| `keys_test.cpp` (11) | open, narrow, create, symlink, size and replace-if-unchanged cases move to `ear_runtime` (`ConfinedStores`) as store cases over a declared rule; the record's own cases (`whyNotKeyRecord`, `load_blob`, `save_blob`, `key-write-failed`) stay, over a harness store; "making the node type narrows every key file" becomes `ConfinedStores::sweep` over sendspin's declaration |
| `settings_test.cpp` (6) | the type case runs through `TypeHarness<spec, Sendspin>`; parse cases unchanged; in 3d, each row's mark |
| `clock_test.cpp` (4) | builds the run's parts with `ear_runtime`'s real adapters beside its real engine (as it builds `FilterPortBlocks` today, `clock_test.cpp:270-282`) [V] |
| new `type_test.cpp` | through the harness: listening asks `claim::TcpListen{8928}`, dialling asks none; a held claim fails with today's text; a distrusted store fails with today's text and is not written; the worker's first timer starts the client only once `start` ran and the clock advanced; close runs the goodbye on the worker after the notifier let go, with no source left. In 3d: a port change claims the new port, unclaims the old at the end, pauses and resumes the sink, and keeps the store's text and the loops; a server URL change posts a redial and is `applied`; a refused new port restarts; a stop during the swap ends with no source left |
| `test/nodes/all_types.hpp` | `notYetInHarness` deleted; `allTypes()` calls `allNodeTypes()`; the config folder and `BootLog` go |
| `test/system/*` | unchanged lines; S3c.6 checks every sendspin system test still passes on the new order of states (8); S3d.2 adds the in-place changes |

## 13. Interim seams, cleared

| Seam (section 2) | Cleared by |
|---|---|
| sendspin off `TypeSpec` / `TypeHarness` | section 12; `NodeTypeOf` then has no production user and stays for tests' hand-made types |
| `NodeFolders`, `NodeFolders::log`, the folder argument | stores (6.8): `node_folders.hpp` deleted; the boot sweep is `StoreAdapter::sweep`, called by `main.cpp` |
| `InsidesEvents` and `Channel` behind `BuildContext::events()` | the run handle (5) |
| Claims in sendspin's type; `rtp` and recordings unclaimed | claims (6.9), ear-wide |
| A leaked run holds its port or file | claims and the leaked-run record (6.10): abandoned ports re-probed; stores and recording paths refused until a late close, which starts the refused node (R15) |
| `file` does I/O on the main loop | recordings open on the I/O executor (6.11); the WAV header in `begin` on the sink's thread (6.6) |
| `notYetInHarness = {"sendspin"}` | section 10: deleted |
| sendspin's own threads, eventfds, note pacing, key file handling | loops (6.4), the framework's wakes (6.3), the run handle (5), stores (6.8) |
| The library's unattributed lines | the log patch (section 9) |
| Every sendspin settings change restarts it | its `reconfigure` (4.2) on 3d's path |

## 14. The boundary with slice 5

Slice 3c fixes the seams; slice 5 (control-plane.md revision 3) adds behaviour on them. Each fact has one home: the
first column's facts are this page's, the second's are control-plane.md's. The last column records what the two pages'
reconciliation in review round 3 settled.

| Slice 3c delivers (this page) | Slice 5 builds on it (control-plane.md) | Reconciled (round 3) |
|---|---|---|
| `RunHandle`: `node`, `generation`, `lost`, `log`, interim `say` paced per run (kept across a reconfigure), reached by `bx.handle()` in `build` and `rx.handle()` in `reconfigure` (G17) | `Report<spec>` over the handle (status writes, events), reached from `BuildOf`; removes `say` once sendspin's notes are split | done: `bx.handle()`, `bx.report()`, `BuildOf` (control-plane.md 5.3) |
| `SetupOf<spec>` and `BuildOf<spec>`'s members of 6.1; the lifecycle's hooks | the realtime ring the framework makes per run, never acquired; the code writes it through `io.set`, `io.raise`, `io.log` | done: `gain`'s `build` returns `Built<>{GainCode{}}` (framework page 6.10) |
| Loops declared in the spec; `RunLoop::postFromOutside` and `closeToOthers` at step 1; own work (a reconfigure's continuation among it) accepted until 4b; loops alive through a reconfigure | action calls posted with `postFromOutside`; a queued call answered `restarted` or `stopping` at step 1; status fields bound to a loop by `writer::loop`; during a reconfigure, actions keep running on the run's loops | `restart` ends a reconfigure in flight (framework page 6.3 and its gate B6, which absorbed control-plane.md X4) |
| "A thread a library owns" as the only non-loop thread (7) | status written by the code (`writer::rt`) or a loop of the spec; a library thread posts to the loop that owns the field | — |
| Diagnostic codes and texts: `ear.loop-slow`, `ear.loop-source-left` (6.4), `ear.claim-wait` (6.9), `ear.close-late`, `ear.close-overdue` with loops (6.10) | puts them on the wire (`node.diagnostic`) and in snapshots | done (control-plane.md 4.6) |
| Counters: a sink's paused frames (6.6) | samples them beside the framework's drops and shortfall (framework page 8.2) | done: per companion (control-plane.md 4.10) |
| `notes-dropped <n>` as the framework's note | `ear.events-dropped` diagnostic | — |
| `log` records with `node` and `run` from a run's handle | `run` on other record kinds; `log.subscribe` later | — |
| `TypeSpec`'s whole member list and order (6.13) | `.status`, `.events`, `.actions`, `.logCodes` after `.lifecycle` | done: cites 6.13 |
| The harness's fakes, by handle, and its clock rule (10) | captures status, events, actions and diagnostics; `h.invoke<action>(args)` on the harness's loops | — |

Not slice 3c's: `restart` and every action, status tables, the wire schema, `typeSchemas`, parameters (slice 6),
recovery (slice 8), the lifecycle's states on the wire (`phase`, `applied`, `companions`). A store does not appear on
the wire.

## 15. The cut: buildable steps

Each step keeps every test green and changes no audio path. Slice 3c's steps assume 3a and 3b built as the framework
page's 13.1 and 13.2 say; S3d.1 and S3d.2 come inside slice 3d, after its reconfigure path (`ReconfigureOf`, the
plan's outcomes) exists.

| Step | What | Tests it adds |
|---|---|---|
| **S3c.1 Run handle** | `RunHandle` replaces `InsidesEvents` and `Channel`; `bx.handle()`; `NodeHost::logTo`; `say` paced in core per run (sendspin's `NoteGate` stays until S3c.6, so its notes are paced twice, harmlessly) | a run's log carries node and run; a log after detach is dropped; a goodbye note is heard; pacing 32 then 1/s with `notes-dropped`, per run; `lost` never held back |
| **S3c.2 Registry and I/O thread** | `ResourceAdapters`; `IoThread` with shared-state jobs and its stop; `ear_refuse_node_type_links` refuses `ear_runtime` and `ear_pipewire` | a null adapter refuses its kind; a job's completion for a gone run dropped and its descriptor closed; the I/O thread busy at stop; the link refusal on each package |
| **S3c.3 Loops** | `loop.hpp`, `Loops`, `TypeSpec::loops`, `StrandPool`, `DedicatedLoops`; staging until `start` returns; `postFromOutside`, `closeToOthers`; teardown 4a/4b; the source count; `Loop::onWake` on `EventWake` (the source's played wake loses its interim `fd()`, U14); the harness's manual loops and clock | post order; `after`, `every` (coalescing), `onWake` (a pending wake fires once on arming), `onReadable`; sources armed in `build` or `start` run only after `start` returned, and never after a failed `build`; queued work destroyed unrun at close; an in-flight handler finishes; outside work refused from step 1; a source left armed at 4b; a dedicated loop that never drains is leaked with "1 loops open"; `ear.loop-slow`; a stuck strand gets a replacement thread; the blocking assert on a strand; the harness's stop failing at once with nothing to turn; the wake's drain-then-clear order (harness hook and real-thread stress) |
| **S3c.4 Stores** | `StoreRule`, `Stores`, `TypeSpec::stores`, `NodeType::stores()`; `ConfinedStores` with sweep, open at readiness step 2, serialised `replace` and `replaceLater`, the 5 s deadlines | store open (missing, created, symlink, too big, distrusted with the file untouched, mode narrowed before judging); `replace` refused when changed; two writes in flight both commit in order; mode narrowed before a write; sweep over declared stores; open past its deadline and its late completion dropped; one compile test of every `TypeSpec` subset spelling (U1); compile-fail cases: two stores of one suffix, a bad suffix, `maxBytes` left out, a mode beyond 0777, `bx.store` and `bx.loop` of a handle the spec lacks |
| **S3c.5 Claims, recordings, consumers' `begin`** | `claim::TcpListen`, `claim::Udp`, `ClaimRegistry` at readiness step 1, `KernelPortProbe` (UDP without SO_REUSEADDR); implicit store, recording and RTP receiver claims; recordings deferred onto the I/O executor; `SinkConsumer::begin` and readiness step 4'; `file` opens its WAV in `begin`; the leaked-run record, late close, auto-start (R15); `ear.claim-wait`, `ear.close-late` in `output-records.json` | granted; refused (running holder, across types) before any open or thread; waited (a removed node on 8928 and an added one on 8928 in one plan; `rec` removed and `rec2` added on one path) with `ear.claim-wait`; the 15 s deadline; a claim held by another node's going old run (a restart's, a failure's) waits, so two nodes whose restarts swap ports both run, with their teardowns finishing in either order; an abandoned port re-probed; a store and a recording path refused while a leaked run holds them, then freed by its late close and the node running by itself (a system test: a peer that never answers at stop, its goodbye finishing after the deadline, the same node then running); a run leaked in step 3 told to close; a run torn down while waiting leaves the queue; `begin` failing and `begin` past 5 s; probe cases moved; system tests for visible changes 2 and 3; `file`'s exact-samples and header tests unchanged |
| **S3c.6 sendspin moves** | section 12 (3c's column); `node_folders.hpp`, the folder argument and `notYetInHarness` go; the thread guard | `type_test.cpp` (3c's cases); sendspin in the all-type harness test; the guard on each spelling, a look-alike and the `/tests/` exclusion; every system test (U4) |
| **S3c.7 The log patch** | `cmake/sendspin-log.patch`; `sendspin_patched_dep` with a patch list; the run object sets the sink and context | `ear_carried_patch_check` for both patches; a library line from the worker and one from the sync thread arrive as `log` records with the node and run (U2); two nodes' lines told apart; a line outside any scope is unattributed from `sendspin-cpp` |
| **S3c.8 Words** | AGENTS.md (architecture, threads, adding a type, tests, sendspin's library row, `rtp`'s held-port line, `file`'s recording line), the framework page's "Slice 3c as built", the `sendspin` skill | — |
| **S3d.1 Primitives in a reconfigure** (inside slice 3d, after `ReconfigureOf`) | `rx.claim`, `rx.unclaim`, the run's own key, the claim wait against the reconfigure's deadline, the break-first rule for a same-port RTP receiver; `rx.recording`; `whenReady` and `ReconfigureDone`; `SinkControl::pause` and `resume`; the source writer's hand-over check in debug builds; the harness's `reconfigure` with these | a claim swapped make-before-break (both held, the old released last); a refused new claim destroys the continuation and restarts; two nodes swapping ports both restart and both run; a same-key replacement granted without a probe and made break-first; a continuation runs only once every acquisition is ready, on the loop named; a token dropped fails the reconfigure; a stop before the post destroys it, after the post lets it run before the goodbye; a newer change before the post (T15) destroys it and releases its claim, after the post waits; pause waits for a consume in flight, discards and counts, posts `paused`; a paused sink at teardown finishes; a second writer without a hand-over asserts |
| **S3d.2 sendspin in place** (only if G19 is answered (a); with (d) it is skipped and sendspin's rows stay restart-only) | sendspin's rows marked `.inPlace()`; `reconfigure`, `redial`, `swapClient` (4.2); the advertiser re-opened | `type_test.cpp`'s 3d cases; system tests: a listening node's port changed while it plays (`reconfiguring: port`, notes, same `id`, the key file not read again, the stream ended and Music Assistant's test server reconnecting to the new port, no `restarting`); a name changed (the server sees the new name after it reconnects, same id); a server URL changed (a redial, no new client); a change to a held port restarting and failing with today's text; dialling to listening and back (U11, U12) |

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
- **Leaked runs keep stores and paths for longer** than today (refused until a late close), so a node behind a library
  that never finishes its stop stays failed until ear restarts; a late close restarts it by itself (R15).
- **Claims are by port, not by address.** Two `rtp` nodes on one port with different multicast groups are refused,
  though PipeWire binds each to its group (`module-rtp-source.c:265-309`) [V]. Multicast is untested (AGENTS.md); a
  later key `udp:<group>:<port>` can relax it.
- **sendspin's in-place change is a new client.** Its connection drops and Music Assistant reconnects (seconds); what it
  saves against a restart is the key file's re-read, the loops' threads, the companions, the run's generation and its
  routes' audio from the source; an operator still hears the stream stop and start (U12).
- **A pause discards what the node sends** during a swap; a consumer that wanted the audio queued cannot have it.
- **A claim wait in a reconfigure** can cost up to the reconfigure's deadline and then the restart's own 15 s, about
  25 s before the node fails.

## 17. Rejected alternatives

| Alternative | Why not |
|---|---|
| Loops on `pw_thread_loop` in `ear_pipewire` | A loop has nothing to do with the engine; strands need a pool `pw_thread_loop` does not give; every threaded type's tests would start PipeWire's loop library (G1) |
| The run object keeps owning its threads (no loop primitive) | Against Q5; each type rewrites stop, wake and poll |
| Loops asked for in `setup` by a runtime name | A name checked only at run time, and slice 5's `call<..., Loop>` checked only at start (G14) |
| A recipe lambda for spec types | Every threaded type repacks resources by hand and a dangling capture compiles (G15) |
| Kept values, a feed and a late handoff consumer (revision 2) | Made necessary only by the two-phase start (R13, R14; 3.1) |
| Staging released when `build` returns (revision 2) | Handlers would run before the code drives and before `start`; the framework page's 6.4 says nothing of the run object's runs before `start` |
| A reconfigure that reports readiness by callback per kind, or a second hook `reconfigured(...)` | A callback per kind is plumbing every type writes; a second hook on the main loop cannot block, and sendspin's swap must run on its worker. `whenReady` is the reconfigure's `build` (G20) |
| The type's own lock around what its consumer writes into | Every in-place type writes it, and a mistake deadlocks the sink's thread against the type's loop; one framework hand-over is tested once (G21) |
| Replacing the sink companion to change what its consumer writes into | Two engine nodes and two rings for a moment, both fed, to move one pointer |
| Restart for sendspin's listening/dialling switch (the framework page's 6.6 estimate) | The library needs a new client for a name or port change anyway (its configuration is fixed when made, `client.h:333-335` [V]); a new client that dials costs nothing more (G19) |
| A same-key replacement probed like a new claim | Its own run holds the key, so the probe fails against itself |
| Wakes for types now (`wake::named`, revision 2) | No type rings one after 3b; a handle no type uses is machinery to test and keep (G23) |
| Stores opened by `build` or the run object | I/O on the main loop, or a run object that reads before it is ready |
| Asynchronous store writes only | The library's `save_blob` returns a `bool` synchronously on its thread (G7) |
| A recording claim judged inside `setup`, the open kept there | No wait is possible, so I8's remove-and-add of one path cannot work, and `file` keeps I/O on the main loop (G16) |
| Claims judged after stores open | The open would read, narrow or create the very file a leaked run still holds |
| Claims per type, as sendspin's | A port is ear-wide |
| The probe alone, no registry | Loses "held by node <n>", and refuses a remove-and-add of one port in one plan |
| A closed source writer that returns 0 at once to every caller | sendspin-cpp's sync thread paces itself on the sink blocking and would spin from step 1 to 4a |
| Passing the client at each `SS_LOG*` site | 533 sites: a patch no bump could carry (G8) |

## 18. Gate questions

The page is written to the recommendations. Answered questions are recorded first; each open question gives plain
background, a concrete scenario, the options and a recommendation with its reason.

### 18.1 Answered, superseded or settled

| # | Question | Answer | Where it lands |
|---|---|---|---|
| G1 | Where do loops, wakes, the I/O executor, the probe and stores live? | The owner: (a), a new `ear_runtime` package | 11 |
| G14 | How does a type get its loops? | The owner: (a), declared in the spec as handles | 6.4 |
| G15 | How is the run object made? | The owner: (a), a static `build` reaching resources by handle | 4.1, 6.1 |
| G17 | The run handle's accessor | The owner: (a), `ready.handle()`; now `bx.handle()` | 5 |
| G3 | A store, recording path or port held by a leaked run | The owner: (a), R15 | 6.9, 6.10 |
| G2 | A mailbox: resource kind or kept value? | Superseded by R13: the run object owns it (framework page 6.5) | 3.1 |
| G4 | A handoff whose consumer is part of the run object | Superseded by R14: companions (framework page 9.6) | 3.1 |
| G6 | `notYetInHarness` | Settled by the framework page's 13.1: it goes in 3c | 10 |
| G10 | The run handle in slice 3 or 5 | Settled by the framework page's 13.1 and 13.2: 3c | 5 |

### 18.2 Open: the shape of this page

**G18. What this page designs of slice 3d.**
*Background.* Slice 3d is "reconfigure and reshape": settings changed while audio runs. Its states, holds, code swap,
reshape and the hook facades are the framework page's (its sections 6 and 6.10); its gate D2 proposes a further module
design, `node-lifecycle.md`, only for 3a's cut and tests. But a reconfigure also swaps claims, waits for new resources,
and lets a type's thread act; those touch this page's primitives. *Scenario.* The sendspin port change of 4.2 needs a
claim swapped make-before-break, a continuation on the worker once the claim is granted, and the sink paused while the
library client is replaced. *Options.* (a) This page designs the primitives' part of 3d (6.2, 6.6's pause, 6.9's
reconfigure rules, sendspin's `reconfigure`), and the framework page the rest. (b) All of 3d in the framework page or
`node-lifecycle.md`; this page stops at 3c. (c) All of 3d here.
*Recommendation: (a).* Each fact keeps one home: claims and loops are this page's in every state, including
reconfiguring; the state machine and its API are the framework page's. (b) would split a claim's rules across two pages.

### 18.3 Open: reconfigure, as a type's threads see it

**G19. Which of sendspin's settings change in place, if any, in slice 3d?**
*Background.* sendspin's settings are `server` (dial this URL; absent means listen), `port` (where to listen) and `name`
(what Music Assistant shows). The library it uses fixes its configuration when a client object is made
(`client.h:333-335` [V]); a running client can only be told to dial another URL (`connect_to`, `client.h:389` [V]). So
a port or name change needs a new client object either way, and every change, a redial included, drops the connection
and ends the stream: **the audio stops whether the node reconfigures or restarts** (the first review of revision 5
found that the framework page had promised otherwise). What in place still buys (4.2): the run, its status and run
number, its loops and companions kept; the identity kept without reading the key file again; both ports held for a
moment so no other node takes the old one mid-change; `reconfiguring` shown instead of `restarting`. What it costs:
`swapClient`, the sink's pause (G21) and two unknowns, a second client's identity over the same key store (U11) and the
swap's duration (U12). *Scenario.* An operator renames `kitchen`'s display name from "Kitchen" to "Kitchen Left";
later switches it from listening to dialling a server URL. Either way Music Assistant shows the node drop and come
back within a few seconds. *Options.* (a) All three in place: a URL change redials; any other change swaps the client
inside the run (4.2). (b) `name`, `port` and `server` in place, but the listening/dialling switch restarts. (c) Only a
URL change in place (a redial, no new client); the rest restart. (d) All three restart-only in 3d; 4.2 stays the
design for when the library can change its name or port without dropping the connection, or an operator needs the
identity kept across a change.
*Recommendation: (a).* The owner's R17 names a network port change as the case a node handles itself, and sendspin is
the type that will see it most; keeping the run and identity makes the change cheaper and safer than a restart (no key
file read, no window where another node takes the port), even though the audio drops, and the machinery it needs (claims
swapped, `whenReady`) is built for `rtp` and the RTP sender anyway. Once a new client is made, dialling instead of
listening is one field of its configuration. Choose (d) if the extra risk of `swapClient` (U11, U12) is not worth a
change the operator hears the same way: S3d.2 is then skipped, and G21's pause is built with its first other user.

**G20. How does a reconfigure act once its new resources are ready?**
*Background.* A reconfigure hook runs on the main loop, which must never block (I11). It can ask for new things (a port
claim, a recording) that become ready later. sendspin must not start its new client until the new port is granted, and
must start it on its own worker thread, not the main loop. *Scenario.* `kitchen` moves from port 8928 to 8929, and 8929
is still held for a moment by a node being removed in the same edit: the claim waits about a second. *Options.* (a)
`rx.whenReady(loop, work)`: the framework posts `work` with the reconfigure's token to the named loop once everything
the hook asked for is ready, or drops it if anything is refused (6.2). (b) The type returns `later()` and is told of
each readiness by a callback per kind. (c) A second hook, `reconfigured(...)`, on the main loop once ready; the type
posts to its loop from there.
*Recommendation: (a).* It reads as `build` does for a start: the type's code runs once everything is there, where the
type works. (b) is plumbing per kind; (c) adds a hook that can only post.

**G21. How does a type change what its sink's consumer writes into?**
*Background.* A sink companion hands the node's audio, chunk by chunk, to a consumer on the sink's own thread (file's WAV
writer, sendspin's sender to Music Assistant). If the type replaces the thing the consumer writes into while audio runs
(sendspin's library client, an RTP sender's socket), no chunk may be in the middle of being written into the old one.
*Scenario.* sendspin swaps its client (4.2) while the operator's microphone audio is being sent to the server through
`to_server`. *Options.* (a) `SinkControl::pause(loop, paused)` and `resume()`: the framework stops calling the consumer
(discarding and counting chunks), tells the type when no call can run, and the type swaps in between (6.6). (b) The type
guards its target with its own lock on the sink's thread. (c) The type replaces the sink companion with a new one whose
consumer writes into the new target.
*Recommendation: (a)*, built in S3d.1 if G19 is (a), else with its first user (the RTP sender). One hand-over, written
and tested once, and the type's swap reads as a sequence. (b) works and
is what sendspin does today for one pointer (`clientMutex_`, `worker.cpp:368`), but each type would write it, and a
mistake deadlocks a thread; (c) costs two engine nodes and two rings to move one pointer.

**G22. A replacement that asks for a claim its own run already holds.**
*Background.* A claim says which node holds a port. An `rtp` node changing only its `format` keeps its UDP port, so its
replacement receiver asks the same claim the old one holds. Two receivers bound to one port at once may each get some
of the packets, or both get all of them [U13]. *Scenario.* `fpp` changes `format` from `L16` to `L24` on port 5004
while FPP streams. *Options.* (a) The run's own claim is granted without a probe, and the framework replaces such a
receiver break-first (old one gone, then new one made), covered by the hold the type already asks for. (b) The type must
ask for break-first itself (`Order::breakFirst`, framework page 6.6). (c) A same-port change is never in place: the
type answers `restart`.
*Recommendation: (a).* The type cannot be expected to know how PipeWire's receiver binds; the framework knows the key is
the same. The framework holds the receiver's outputs and waits for the fade to reach zero before it destroys the old
receiver, so the gap is a fade, silence and a fade in, never a click. The audio gap is what a restart would cost,
without the node leaving `running`'s family of states.

### 18.4 Open: what stays from revision 2, asked again

**G23. Do types get wakes of their own now?**
*Background.* A wake lets one thread (even the audio thread) tell a loop "look now" cheaply. Revision 2 let a type ask
for named wakes because sendspin's audio code rang one every cycle. After slice 3b sendspin has no audio code; the
source companion rings its own wake, and the framework's status bell (slice 5) is the framework's. *Scenario.* A future
sequencer type whose audio code publishes its position to its run object through a mailbox (the framework page's gate
C3) wants its run object to look at once. *Options.* (a) Defer: `Wake` and `Loop::onWake` stay for the framework's
wakes; `wake::named` and the setup and build calls come with the first type that needs them. (b) Build them now, as
revision 2 designed.
*Recommendation: (a).* Nothing would use them; the design is kept (revision 2's 6.3) and costs one step to add.

**G16. A recording: when is it claimed and opened, and where is its header written?**
*Background.* A `file` node writes a WAV file beneath the recordings folder. Today ear creates or truncates the file and
libsndfile writes its header on the main loop, inside the node's start, before anything could check that another node is
writing the same file. *Scenario.* An operator renames `rec` to `rec2` (a removal and an addition in one edit), both on
`take1.wav`; or adds a second node on `take1.wav` by mistake. *Options.* (a) A deferred kind: the path is claimed first,
the file opened on the I/O thread, and the header written by the sink consumer's `begin` on the sink's thread, before the
node is `running` (6.6, 6.11). (b) The open stays where it is, its claim judged there, refusal only.
*Recommendation: (a).* The rename works (the new node waits for the old to close), the mistake is refused before the
first node's file is truncated, and the main loop never touches the disk; a libsndfile failure is still a failed start,
as today. With (b) the rename is refused.

**G13. How long may a new wait last?**
*Background.* This slice adds three waits: a store's open, a recording's open (on the I/O thread), and a claim held by a
node still closing. A wait without an end would leave a node `starting` for good, which the framework page's I3 forbids.
*Scenario.* The config folder is on a network disk that hangs. *Options.* (a) Opens 5 s; claim waits 15 s (after the
holder's own 10 s close deadline); a consumer's `begin` 5 s. (b) No deadlines until slice 8.
*Recommendation: (a).* Every wait added here is new in this slice; the operator sees `failed ... not opened within 5 s`
instead of a node that never starts.

**G7. How does a store write?**
*Background.* sendspin's library saves its pairings through a callback that must answer, on the library's own thread,
whether the write worked. *Scenario.* A new pairing arrives while the disk is slow. *Options.* (a) `replace`, blocking
up to 5 s, from a dedicated loop or a library thread, and `replaceLater` for any loop, both queued in order on the I/O
thread. (b) Asynchronous only.
*Recommendation: (a).* The library's callback is synchronous; an asynchronous-only store would make sendspin wait on a
future, which is `replace` with extra steps.

**G5 and G11. Notes and boot findings until slice 5.**
*Background.* Today sendspin paces its own `says` notes (32, then one a second) and writes what its boot sweep could not
narrow as `log` records from `sendspin`. Slice 5 turns notes into status and events and puts diagnostics on the wire.
*Scenario.* A flooding server makes a sendspin node print hundreds of `pairing-failed` notes. *Options.* (a) The run
handle paces every type's `say` per run now, with `notes-dropped <n>`; boot findings stay `log` records from the type's
name. (b) Leave both in sendspin until slice 5.
*Recommendation: (a).* sendspin can delete `NoteGate` now, and the boot sweep is the framework's from S3c.4; slice 5
replaces both texts with its own.

**G8. Telling whose node a library's log line is.**
*Background.* sendspin-cpp writes its log lines straight to standard error with no way to say which client wrote them;
with two sendspin nodes the lines cannot be told apart. *Scenario.* `kitchen` and `porch` both lose their server; the
log shows two `connection lost` lines. *Options.* (a) A carried patch: a log hook plus a per-thread context the
library's threads and socket callbacks inherit (about 15 places, section 9). (b) Pass the client at each of 533 call
sites. (c) Leave them unattributed.
*Recommendation: (a).* A patch small enough to rebase at each bump of the pin, and worth offering upstream.

**G9. The strand pool's sizes.**
*Background.* A strand is a loop without a thread of its own, run on a small shared pool, so a hundred nodes do not cost
a hundred threads (R5). A handler that blocks holds a pool thread. *Scenario.* Sixteen RTP senders each run a control
strand; one of them is stuck in a slow handler. *Options.* (a) `min(4, cores)` threads, slow at 100 ms, a replacement
thread at 1 s, at most 16. (b) One thread per strand until a type uses strands.
*Recommendation: (a)*, measured on the Talos CPU once known (U5): one stuck strand delays the others for at most 1 s.

**G12. A guard against threads in node types.**
*Background.* P1 says no type code starts a thread. A review can miss one. *Scenario.* A new type author adds a
`std::thread` to poll a device. *Options.* (a) A configure-time guard refusing `std::thread`, `std::jthread`,
`std::async`, `pthread_create` and `fork` under `src/nodes/` outside `/tests/`. (b) Review only.
*Recommendation: (a).* P1 is then checked on every build, as standard output already is.

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
- **U9.** The boundary with control-plane.md: reconciled with its revision 3 and the framework page's revision 5 in
  review round 3 (section 20). An owner's answer that moves a seam (G18, G19) changes both pages.
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

