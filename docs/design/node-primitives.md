# Node primitives: slice 3 of the node framework

Status: proposal, revision 2, 2026-11-08, after two independent reviews (section 20). Nothing here is built.
Layer: module design, beneath `docs/design/node-framework.md` (revision 4; "the framework page" below). It refines that
page's sections 6, 7.2, 7.3 and 7.4 and the slice 3 line of its section 13, and says in section 3 exactly where it
changes them. Slices 5 and 6 (control plane, parameters) are designed at the same time, by another author, in
`docs/design/control-plane.md`; section 14 here is the boundary between the two pages, and section 6.12 is the one
home of `TypeSpec`'s member list for both.

[V] = verified in the file named, as of `f677a50`; [U] = unverified, with the step or test that checks it (section 19).
Line numbers name the file as it is at that commit. Files of the pinned sendspin-cpp tree (`7be2278`) are cited by
their path in it; PipeWire 1.4.2's source is cited as `pipewire-1.4.2/<path>`.

## 1. The owner's words

| # | The owner's words | Date, where | What it means here |
|---|---|---|---|
| R1 | "what are the core primitives that ear should host so that new nodes can be added simply and only require defining the truly custom bits that its node type demands" | 2026-11-07, chat | The primitives sendspin wrote for itself become the framework's; a type writes only its protocol |
| R9 | "Nodes should not have direct access to stdout or stderr. The core framework should expose the necessary primitives for events, diagnostics, and logging. These should be real-time safe for any audio loops that need them, and there can be non real-time bases for the rest of the system." | 2026-11-07, chat (gate, Q1) | A run's threads log through a handle the framework gives; the library's own lines are attributed to the node (section 9) |
| Q5 | "Strands, dedicated on request" | 2026-11-07, chat (gate) | Loops (section 6.4) |
| Q7 | "Pipe into a feed first" | 2026-11-07, chat (gate) | The feed here is the one the paced feed of slice 9 builds on (section 6.5) |
| P1 | "3 seems like a standalone unit, while 5 and 6 need to be designed together." | 2026-11-08, chat | This page stands alone; it fixes only the seam slice 5 extends (section 14) |
| P2 | "Let's start the design and architecture phase for both. I want a complete proposal or a design doc before implementation. The goal is high UX and DX" | 2026-11-08, chat | Design first; DX is what a type author writes (section 4), UX is what an operator sees when a primitive fails (section 8) |

Design rules 1 to 3 of `pipewire-graph.md` hold, as do the framework page's I1 to I9 and its section 14 answers.

## 2. The problem, measured

Slice 2 left one type off the type interface and five interim seams [V]:

| Seam | Where | Why it is there |
|---|---|---|
| `sendspin` derives from `NodeTypeOf` and writes its own `NodeConfig` | `sendspin_type.cpp:72-130` | Its start builds threads, so it could not take `TypeSpec` and `TypeHarness` |
| `NodeFolders` (config folder and a boot `LogSink*`), and `makeSendspinNodeType(const NodeFolders&)` | `node_folders.hpp:12-18`, `sendspin.hpp:11`, `all_node_types.cpp.in:9-12`, `main.cpp:215` | sendspin's key files and its boot sweep (`sendspin_type.cpp:137-144`) |
| `StartContext::events()` beside `Ready::events()` | `node_type.hpp:201-203`, `226-232`; `node_host.cpp:112-116` | sendspin's worker is built in `start` and keeps the events (`sendspin_type.cpp:57`) |
| A recipe that holds threads built in `start` must ask for no outside node (said, not enforced) | the framework page, "Slice 1 as built", the 7.2/I5 bullet | A recipe that never ran is destroyed on the main loop, never closed (`node_host.cpp:432-443`), so its threads would be joined there |
| A leaked run keeps its port or file while the next run starts | the framework page, "Slice 1 as built", the I5a bullet; `node_host.cpp:447-464` | Claims live in sendspin's type (`port_claims.hpp:12`), not in core; nothing claims a recording path |
| `notYetInHarness = {"sendspin"}` | `test/nodes/all_types.hpp:71-74` | sendspin's start builds its worker |

And sendspin's own copies of primitives any threaded type needs, about 560 lines [V]:

| sendspin's copy | Lines | What it is |
|---|---|---|
| `SendspinWorker` thread, 10 ms `poll()` loop | `worker.cpp:88-94`, `189-215`, `278-299` | a dedicated loop with a periodic timer |
| notifier thread on an `EventFd` | `worker.cpp:309-323` | a dedicated loop watching a wake |
| `EventFd` | `event_fd.hpp` (33) | a wake |
| `Feed` | `feed.hpp`, `feed.cpp` (109) | a feed |
| `Mailbox<T>` | `mailbox.hpp` (48) | a latest-value slot, realtime writer |
| `NoteGate` | `note_gate.hpp`, `note_gate.cpp` (67) | note pacing: 32, then one a second (`note_gate.cpp:11-12`) |
| `PortClaims`, `HeldPort`, `probeListenPort` | `port_claims.*` (60), `sendspin_type.cpp:36-47` | a claim, held by the *type* across its nodes |
| `NodeKeyStore::open`, `narrowKeyFiles` | `key_store.cpp:31-97` | a store: named file, mode 0600, read, trusted, created, swept at boot |

With the glue around them (the old `SendspinRun`, `SendspinConfig`, the worker's thread code), sendspin loses about 650
lines in all (section 12). Section 11 and 16 use that figure.

Three facts shape everything below:

- **The code exists before the run object.** Since slice 1 the run's code is made in `start` and drives the block at
  once; the run object is built later, by the recipe (`node_host.cpp:300-308`). So whatever both share (sendspin's feed,
  progress mailbox, capture mailbox and bell) cannot be a member of the run object. Today sendspin dodges this by
  building the run object in `start` (`sendspin_type.cpp:93-108`). Shared state must be owned by the framework and
  acquired in `start`.
- **ear_core may not touch threads, files or sockets** (AGENTS.md, Architecture). Loops, wakes, feeds that wait, stores
  and port probes are interfaces in core and adapters elsewhere.
- **`file` does I/O on the main loop today.** Its recording is created or truncated inside `start`
  (`node_host.cpp:137-141`, `main.cpp:84`), and `sf_open_fd` writes the WAV header there too (`file.cpp:76`), before
  any claim could be judged. A claim on a path is only real if the open waits for it (section 6.11).

## 3. Decisions, and where this page changes the framework page

In one paragraph: a type's spec declares its **stores** and its **loops** (strands or dedicated threads) as
compile-time handles, beside its banks. Its `start`, through a spec-typed `StartOf<spec>`, acquires what its code needs
at once (**feeds**, **wakes**, **handoffs**, kept values such as a **mailbox**) and asks for **claims** and
**recordings**. The framework judges claims first, then opens stores and recordings on its **I/O executor**, then
starts loops, then calls the behaviour's `build`, which makes the run object on the main loop from a spec-typed
`ReadyOf<spec>`, with no I/O, no threads and no captures. The run's way out, the **run handle**, belongs to this slice:
`lost`, `log` and, until slice 5 splits notes, a paced `say`. Teardown gains one step (4b: the framework closes the
run's loops after the run object said goodbye), loops stop taking outside work at step 1, and claims become ear-wide.
A leaked run keeps listening, so its late close releases its claims and starts what it held back. Implementations live
in a new package, `ear_runtime`. sendspin moves onto `TypeSpec` and `TypeHarness`, `file` stops doing I/O on the main
loop, and every interim seam of section 2 goes.

| Framework page | Change | Here |
|---|---|---|
| 6, I5 step 1 | Also: every feed closed (a wait already in progress ends; a later one waits its patience), every loop closed to outside work (queued actions answered `restarted` or `stopping`), claim waits and pending opens of a run not yet ready dropped | 6.5, 6.4, 6.10 |
| 6, I5 step 4 | Split: 4a the run object closes (goodbye may run on its loops); 4b the framework closes the run's loops. Before `closed`, a run object has destroyed every loop source it armed | 6.4, 6.10 |
| 6, I5 step 5 | The order made exact: code, run object, recipe, kept values (reverse order of `keep`), feeds, wakes, loops, stores, recordings, claims, config | 6.10 |
| 6, I5a | A leaked run keeps listening: told to close even if it leaked in step 3, and its late close (everything reported) releases its claims, says `ear.close-late` and starts any node its claims refused (gate G3). Its memory stays leaked | 6.9, 6.10, 8 |
| 6, I8 | Claims are judged first, before any store opens or thread starts. A claim on something no probe can check (a store, a recording path) held by a leaked run is refused, not given, until that run's late close. The wait for a going holder has a 15 s deadline and says so as it starts (`ear.claim-wait`). A failed run that was not torn down still holds its claims | 6.9 |
| 6, I8, "mDNS name" | Not claimed: the instance name embeds the first 8 hex digits of the node's client id, which its key file makes per node, so two nodes cannot collide | 6.9 |
| 7.1 | A store is declared with a rule: mode, size bound (required), the text a missing file is created with, and why a text is not trusted. A loop is declared too | 6.8, 6.4 |
| 7.2 | `start` gets `StartOf<spec>&`; the recipe becomes the behaviour's `static build(ReadyOf<spec>&, const CheckedOf<spec>&)`; `Ready::handle()` replaces `Ready::events()`; `StartContext::events()` goes; `ready.attach` hands a handoff its consumer | 5, 6.1, 6.6 |
| 7.3, Recording row | A deferred kind: opened on the I/O executor after its claim, its descriptor reached by `ready.recording(...)` | 6.11 |
| 7.3, Mailbox row | Not a resource kind: a plain core value kept by the run (`cx.keep<Mailbox<T>>()`), since it has no adapter, readiness or loss | 6.2 |
| 7.3, Feed row | A feed declares its own latency on its bank (`[0, capacity]`), so a type never repeats it; `feed{bank, frames}` is spelt `cx.feed(bank, frames)` | 6.5 |
| 7.3, Loop row | `loop::strand`, `loop::dedicated` keep their names, now as spec handles with a name; the dedicated thread is `ear-<name> <node>` | 6.4 |
| 7.3, Port claim row | `claim::tcpListen{port}`, `claim::udp{port}` are spelt `claim::TcpListen{port}`, `claim::Udp{port}` (types, as `SoundCardRequest` is) | 6.9 |
| 7.3, Store row | Writes are `replace` (blocking, from a dedicated loop or a library thread) or `replaceLater` (any loop); both go through the I/O executor in order | 6.8 |
| 7.3, readiness deadlines | Stores and recordings (5 s) and claim waits (15 s) get theirs here; outside nodes' stay with slice 8 | 6.8, 6.9, 6.11 |
| 7.4 | Loop sources armed from `build` are staged until it returns; a strand handler past 100 ms is `ear.loop-slow`; past 1 s the pool adds a thread | 6.4 |
| 7.6 | The harness runs loops by hand with a fake clock; claims, stores, recordings, wakes and feeds are faked, each named by its handle | 10 |
| 8.8, its erratum | The run handle and its node-facing `log` belong to slice 3, not slice 5. The boot sweep's findings stay `log` records from the type's name until slice 5 | 5, 6.8 |
| 12 ("the loop pool, eventfds and stores are behind interfaces in `ear_pipewire` and `ear_folder`") | They are in `ear_runtime`, which links `ear_folder`; `ear_pipewire` gains nothing | 11 |

## 4. Journeys: what a type author writes

### 4.1 A type with its own threads: sendspin

**Before** (today, abridged; every line cited is [V]). The type holds a claims map across its nodes; `start` claims and
probes, then builds the whole run object, which reads the key file and starts two threads, then hands it over through a
recipe that only returns it:

```cpp
class SendspinConfig final : public NodeConfig {                       // sendspin_type.cpp:72
	StartResult start(StartContext& context) const override {
		std::optional<HeldPort> port;
		if (!settings_.server) {                                          // :83-92: claim, then probe the kernel
			if (auto claimed = claims_->claim(settings_.port, node_); !claimed) return refused(claimed.error());
			port.emplace(claims_, settings_.port);
			if (auto probed = probeListenPort(settings_.port); !probed) return refused(probed.error());
		}
		auto run = std::make_unique<SendspinRun>(folder_, node_, settings_, context, std::move(port)); // threads start
		RtWriter& toServer = context.acquire(HandoffRequest{"to", rate / 2, &run->toServer()});
		context.declareLatency("from", Latency{0, feedFramesFor(context.format())});
		auto code = run->code(rate, toServer);                            // reads the run's feed, mailboxes and bell
		return StartResult{std::move(code), [run = std::move(run)](Ready&) mutable { return std::move(run); }};
	}
};
// worker.cpp:189-215: the worker thread opens the key file (NodeKeyStore::open), makes the client, starts the
// notifier thread, then polls every 10 ms until stop; finish() joins the notifier, stops the client, calls `stopped`.
// makeSendspinNodeType(const NodeFolders&) sweeps the folder and writes what it could not narrow to the boot log.
```

**After.** The spec declares the banks, the key file and the two loops; `start` only acquires what the code reads;
`build` makes a run object that holds the protocol and nothing else. Nothing travels from `start` to `build` by hand:
`build` reaches every resource by its handle, and the settings are its argument.

```cpp
// src/nodes/sendspin/sendspin_spec.hpp
namespace ear::nodes::sendspin {
inline constexpr auto to = bank::input<"to">;
inline constexpr auto from = bank::output<"from">;
/// sendspin-<node>.key in the graph file folder: mode 0600, at most 64 KiB, created empty, trusted only as a key record.
inline constexpr auto keyFile =
	store::file<"key">({.mode = 0600, .maxBytes = 64 * 1024, .initial = &emptyKeyRecord, .distrust = &whyNotKeyRecord});
inline constexpr auto worker = loop::dedicated<"sendspin">;   // the library's main loop: start, poll, every callback
inline constexpr auto notifier = loop::dedicated<"notify">;   // apart: a late notify costs a sync
inline constexpr auto played = wake::named<"played">;         // the code rings it when it played something
inline constexpr TypeSpec spec{.settings = settingsTable, .banks = Banks<to, from>{}, .stores = Stores<keyFile>{},
                               .loops = Loops<worker, notifier>{}};

struct Sendspin {
	static Members<spec> ports(const CheckedOf<spec>&) {
		return {to(members("left", "right")), from(members("left", "right"))};
	}
	static StartResult start(StartOf<spec>& cx, const CheckedOf<spec>& checked);
	static BuildResult build(ReadyOf<spec>& ready, const CheckedOf<spec>& checked);
};
}

// src/nodes/sendspin/sendspin.cpp
StartResult Sendspin::start(StartOf<spec>& cx, const CheckedOf<spec>& checked) {
	if (const SendspinSettings settings = settingsFrom(cx.node(), checked); !settings.server)
		cx.claim(claim::TcpListen{settings.port});                  // judged first: refuses, waits (I8), then probed
	const std::uint32_t rate = cx.format().rate;
	return SendspinCode{cx.feed(from, feedFramesFor(cx.format())),  // declares [0, n] on "from" itself
	                    cx.keep<Mailbox<PlayProgress>>(), cx.wake(played), rate,
	                    cx.handoff(to, rate / 2),                    // its consumer is attached by build
	                    cx.keep<Mailbox<CaptureAnchor>>()};
}
BuildResult Sendspin::build(ReadyOf<spec>& ready, const CheckedOf<spec>& checked) {
	// The store's rule already trusted the text (whyNotKeyRecord said nothing), so parsing it cannot fail; no I/O here.
	KeyRecord record = KeyRecord::parseTrusted(ready.store(keyFile).text());
	auto run = std::make_unique<SendspinRun>(ready, settingsFrom(ready.node(), checked), std::move(record));
	ready.attach(to, run->toServer());
	return run;
}
std::unique_ptr<const NodeType> makeSendspinNodeType() { return makeNodeType<sendspin::spec, sendspin::Sendspin>(); }
```

The run object takes what it uses from `ready`, arms its loops in its constructor (staged until `build` returned, 6.4)
and says goodbye on them, destroying every source it armed before it calls `closed`:

```cpp
SendspinRun::SendspinRun(ReadyOf<spec>& ready, SendspinSettings settings, KeyRecord record)
	: settings_(std::move(settings)), handle_(ready.handle()), keys_(ready.store(keyFile), std::move(record)),
	  feed_(ready.feed(from)), progress_(ready.kept<Mailbox<PlayProgress>>()), worker_(ready.loop(worker)),
	  notifier_(ready.loop(notifier)), toServer_(ready.format().rate, ready.kept<Mailbox<CaptureAnchor>>()) {
	starting_ = worker_.after(std::chrono::milliseconds{0}, [this] { startClient(); }); // client.start, on the worker
	tick_ = worker_.every(std::chrono::milliseconds{10}, [this] { tick(); });            // client.loop, watch, mDNS
	notified_ = notifier_.onWake(ready.wake(played), [this] { notify(); });             // notify_audio_played
}
void SendspinRun::close(std::move_only_function<void()> closed) {
	// The notifier lets go of the player first, then the worker stops the client (its goodbye) and calls `closed`.
	notifier_.post([this, closed = std::move(closed)]() mutable {
		notified_.reset();                  // on its own loop: no notify runs after this line
		worker_.post([this, closed = std::move(closed)]() mutable {
			starting_.reset();              // a client not started yet is never started
			tick_.reset();
			stopClient();                   // client->stop(), then the client destroyed: its threads joined, on the worker
			closed();
		});
	});
}
// The library's sync thread, when the server streams a format ear did not ask for (worker.cpp:124-131 today):
std::size_t SendspinRun::on_audio_write(std::uint8_t* data, std::size_t length, std::uint32_t timeoutMs) {
	if (!streamAsked_.load(std::memory_order_acquire)) {
		feed_.idle(std::chrono::milliseconds{timeoutMs});   // waits its patience: no spin, before or after the close
		return 0;
	}
	/* as today: convert, then feed_.write(scratch_, frames, std::chrono::milliseconds{timeoutMs}) */
}
```

What the author no longer writes: a thread, a poll loop, an eventfd, a mailbox, a feed, note pacing, a claims map, a
probe, a `HeldPort`, a key file's open, mode or sweep, a `NodeConfig`, a `NodeTypeOf`, `declareLatency` for the feed,
a folder parameter, or a lambda that carries resources from `start` to the recipe. What stays: settings, the protocol
callbacks, the time filter's dating in `process`, the key record's format, mDNS, pairing notes (until slice 5).

### 4.2 A type that needs a store and a claim

The example is an RTP *sender* (rtp.md section 9 lists it as not built), kept small. It sends its input bank as L24 to a
unicast address from a fixed source port, which no two nodes may share (a claim), and keeps its SSRC across restarts so
a receiver does not see a new stream each time (a store). It is an illustration of the interface, not a type this page
asks for.

**Before** (what slice 2's interface would make an author write today): a folder argument, which only sendspin may take
(`all_node_types.cpp.in:9-12`); a type-held claims map with a `HeldPort` and a probe, copied from sendspin; the SSRC
file read and created in `start` on the main loop (against 7.4) or on a thread the type starts itself, with its own
eventfd to stop it; its own mode handling; its own sweep at type construction; and the run object built in `start`, so
it may ask for no outside node (section 2). About 200 lines before the first packet.

**After** (complete; `parseIpv4`, `encodeL24` and `RtpHeader` are the type's own few lines). One thread owns the
socket, the handoff's: it opens it when the consumer is attached, sends every packet and every RTCP report from
`consume`, and sends the BYE and closes it in `finish`. Nothing else touches the descriptor, so there is nothing to
synchronise.

```cpp
// src/nodes/rtpsend/rtpsend_spec.hpp
namespace ear::nodes::rtpsend {
std::string newSsrc();                                              // eight random hex digits
std::optional<std::string> whyNotSsrc(std::string_view text);      // "malformed: ..." unless eight hex digits
namespace rows {
inline constexpr auto address = setting::required("address", setting::text<parseIpv4>("an IPv4 address"));
inline constexpr auto port = setting::required("port", setting::number(1, 65535));
inline constexpr auto sourcePort = setting::defaulted("sourcePort", setting::number(1, 65535), 5006);
inline constexpr auto channels = setting::required("channels", setting::number(1, 8));
}
inline constexpr SettingsTable<rows::address, rows::port, rows::sourcePort, rows::channels> settings{"rtpsend"};
inline constexpr auto send = bank::input<"send">;
inline constexpr auto ssrcFile =
	store::file<"ssrc">({.mode = 0644, .maxBytes = 64, .initial = &newSsrc, .distrust = &whyNotSsrc});
inline constexpr TypeSpec spec{.settings = settings, .banks = Banks<send>{}, .stores = Stores<ssrcFile>{}};

class Sender final : public RunObject, public HandoffConsumer {
public:
	Sender(const CheckedOf<spec>& s, RunHandle& handle, std::uint32_t ssrc);
	// The handoff's thread, all three. attached(): socket, bind sourcePort, connect; a failure is handle_.lost(...).
	void attached(std::uint64_t skippedFrames) override;
	void consume(std::span<const float> interleaved, std::uint32_t frames) override; // send; RTCP every 5 s of audio
	void finish() override;                                                          // BYE, then close the socket
	void close(std::move_only_function<void()> closed) override { closed(); }       // nothing of its own to stop
private:
	RunHandle& handle_;
	int socket_ = -1;          // the handoff's thread only, from attached() to finish()
	std::uint32_t ssrc_;
};

struct RtpSend {
	static Members<spec> ports(const CheckedOf<spec>& s) { return {send(members(s.get<rows::channels>()))}; }
	static StartResult start(StartOf<spec>& cx, const CheckedOf<spec>& s) {
		cx.claim(claim::Udp{s.get<rows::sourcePort>()});      // takes the checked number; 1 to 65535 or logic_error
		cx.handoff(send, cx.format().rate / 10);
		return RunCode::toHandoff(send);                        // writes the bank into its handoff, nothing else
	}
	static BuildResult build(ReadyOf<spec>& ready, const CheckedOf<spec>& s) {
		auto sender = std::make_unique<Sender>(s, ready.handle(), parseSsrc(ready.store(ssrcFile).text()));
		ready.attach(send, *sender);
		return sender;
	}
};
}
```

The SSRC file is `rtpsend-<node>.ssrc` (I9), created with `newSsrc()` on first start, narrowed to 0644 at boot and at
each open, and a corrupt one fails the node `start-failed: rtpsend-<node>.ssrc: malformed: ...` without being
replaced. A second node on the same source port fails `start-failed: cannot bind UDP port 5006: held by node <n>`;
removing one node and adding another on that port in one plan works, the new one starting when the old one let go (I8),
with `node <new> ear.claim-wait: udp:5006 held by node <old>, which is closing` while it waits.

`RunCode::toHandoff(bank)` is the framework's code for a type that only takes audio out (6.6): explicit, refused at
compile time for an output bank, and the only way a bank reaches a handoff without the type's own `process`.

## 5. The run handle (slice 3, not slice 5)

**Decision: the run handle belongs to slice 3.** The framework page's slice 4 erratum (8.8) defers the node-facing log
calls "on a run handle" to slice 5. This page takes the handle and its `log` into slice 3, and leaves to slice 5 what it
builds over the handle (the typed `Report<spec>` for status writes and events, control-plane.md 5.3) and beside it (the
realtime ring the framework makes per run, written by `io.set`, `io.raise` and `io.log`, control-plane.md 5.4, 6.1).
Reasons:

- Slice 3 moves every thread a type has onto framework loops, and each loop handler needs a generation-checked way out;
  the handle is that object, and its lifetime rules (I6) are slice 3's teardown rules.
- The carried sendspin-cpp patch (section 9) attributes the library's lines to a node and run. It needs a node log that
  carries `node` and `run`, which `LogEntry` already holds (`log.hpp:17-25`).
- `StartContext::events()` and `Ready::events()` must collapse into one thing now (section 2); naming it once avoids a
  second rename in slice 5.

```cpp
// ear/core/run_handle.hpp: replaces InsidesEvents (node_type.hpp:174-180)
/// One run's way out, from any of its threads; never blocks. Shared-owned by the framework, so it outlives every thread
/// that can reach it, a leaked run's included. What it carries lands only on this run (I6).
class RunHandle {
public:
	virtual const NodeName& node() const noexcept = 0;
	virtual std::uint64_t generation() const noexcept = 0;   // the `run` of its records
	/// A fact: the run cannot go on. The node fails start-failed with `detail`. Ignored once the run's close began.
	virtual void lost(std::string detail) = 0;
	/// Free text for people (8.8). The framework sets `node` and `run`; an empty `source` is the type's name. Straight to
	/// the log sink (any thread, never blocks), paced per node by the encoder as today. Dropped once the run is detached;
	/// a line racing the detach may land just after it, carrying the old `run`.
	virtual void log(LogEntry entry) = 0;
	void debug(std::string text) { log({.level = LogLevel::debug, .text = std::move(text)}); }
	void info(std::string text) { log({.level = LogLevel::info, .text = std::move(text)}); }
	void warn(std::string text) { log({.level = LogLevel::warn, .text = std::move(text)}); }
	void error(std::string text) { log({.level = LogLevel::error, .text = std::move(text)}); }
	/// Interim, until slice 5 splits notes into status and events: `node <n> says <text>`, on the main loop in order.
	/// Paced per run (a restart starts afresh, as NoteGate per worker does today): 32 at once, then one a second; the
	/// next note that passes is preceded by `notes-dropped <n>`. Heard until the run is detached, so a goodbye note is
	/// heard.
	virtual void say(std::string text) = 0;

protected:
	~RunHandle() = default;
};
```

- **Order.** `lost` and `say` cross to the main loop in order (as `Channel` does today, `node_host.cpp:173-226`), so a
  note is ordered with node states. `log` goes straight to the sink: it is not ordered with states, as captured output
  is not today. Its detach check is an atomic flag.
- **Pacing.** `say`'s pacing is sendspin's `NoteGate` (`note_gate.cpp:11-12`) moved into core, with the same text, now
  the framework's for every type, one gate per run. `lost` is never held back.
- **Where it is reached.** `ready.handle()` in `build`; the run object keeps the reference. `start` gets none: it only
  acquires, so it has nothing to tell.
- **The sink.** `NodeHost::logTo(LogSink&)`, wired in `main.cpp` to `StatusLog` (the same sink captured output uses).
  `status_log.cpp:197` already writes `run` when present [V].
- **A leaked run's handle** stays detached from the node but attached to the leaked run's record (6.10), which hears
  its late `closed` calls; `lost`, `log` and `say` from it are dropped as before.

## 6. The primitives

### 6.1 `StartOf<spec>`, `ReadyOf<spec>`, `build`, and readiness

A behaviour sees the run through two spec-typed facades. They are thin templates over core's virtual `StartContext`
and `Ready` (made by `SpecConfig`, `type_spec.hpp:318-336`), so the erased interfaces stay as they are for hand-made
`NodeTypeOf` types and every refusal that can be made at compile time is.

```cpp
// ear/core/type_spec.hpp
template <const auto& Spec> class StartOf {
public:
	AudioFormat format() const;
	const NodeName& node() const;
	/// A feed into an output bank of the spec (routable or inside); its reader for the code. Ready at once (6.5).
	template <class Bank> FeedReader& feed(Bank, std::uint32_t capacityFrames);       // static: output bank of Spec
	/// A handoff from an input bank of the spec; its writer for the code. The consumer is build's (6.6).
	template <class Bank> RtWriter& handoff(Bank, std::uint32_t capacityFrames);      // static: input bank of Spec
	/// A wake, by its handle; ready at once (6.3). One per handle per run (std::logic_error for a second).
	template <class Wake> Wake& wake(Wake);
	/// A recording file, opened later (6.11). One per path per run.
	void recording(RecordingRequest request);
	/// A claim, judged before anything else is readied (6.9).
	void claim(claim::TcpListen);
	void claim(claim::Udp);
	/// A value the run keeps (6.2): made now, freed with the run after its run object, leaked with it.
	template <class T, class... Args> T& keep(Args&&... args);
	/// Outside nodes, unchanged from slice 2 (their bank given by handle or name).
	void acquire(const SoundCardRequest&);
	void acquire(const RtpReceiverRequest&);   // now also claims udp:<port> (6.9)
	void declareLatency(BankRef bank, Latency internal);   // unchanged
};

template <const auto& Spec> class ReadyOf {
public:
	AudioFormat format() const;
	const NodeName& node() const;
	RunHandle& handle();                                                // replaces events()
	template <class Store> Store& store(Store);                         // static: in Spec.stores; open and trusted (6.8)
	template <class Loop> Loop& loop(Loop);                             // static: in Spec.loops; running (6.4)
	template <class Bank> FeedWriter& feed(Bank);                       // static: output bank; start asked for it
	template <class Wake> Wake& wake(Wake);                             // start asked for it
	template <class T> T& kept();                                       // the one value of type T start kept
	int recording(std::string_view path);                               // its descriptor, open (6.11)
	/// The consumer of the handoff on `bank`, once (6.6).
	template <class Bank> void attach(Bank, HandoffConsumer& consumer); // static: input bank of Spec
};

using BuildResult = std::expected<std::unique_ptr<RunObject>, FailureReason>;
// The behaviour:
//   static StartResult start(StartOf<spec>&, const CheckedOf<spec>&);            required
//   static BuildResult build(ReadyOf<spec>&, const CheckedOf<spec>&);            optional: no run object without it
```

- **`build` replaces the recipe lambda for spec types.** It is found by name and held to its exact signature, as
  `check` is (`type_spec.hpp:365-374`). A spec type's `start` that returns a `StartResult` carrying a recipe throws
  `std::logic_error` at start (a type bug the harness catches): one way to build a run object, not two. Hand-made
  `NodeTypeOf` configs keep `StartResult`'s recipe and the erased `Ready`.
- **Look-ups that fail** at run time (`ready.feed(b)` for a bank start gave no feed, `ready.kept<T>()` for a type
  start did not keep, `ready.wake(w)` for a wake start did not ask for, `ready.recording(p)` for a path it did not ask
  for, a second `cx.keep<T>()` of one `T`) throw `std::logic_error` naming the node and the handle: a type bug, found by
  the type's first harness test.
- **Every `cx.` call runs inside `start` only**, on the main loop, and throws `std::logic_error` outside it
  (`node_host.cpp:126-127`); every `ready.` call runs inside `build` only.
- **Stores and loops are not asked for**: the spec declares them, so every run of the type has them.
- Kinds that become ready later tell the framework through one small interface, main thread only, the same for loops,
  stores and recordings as `OutsideNodeEvents` is for outside nodes:
  ```cpp
  class ReadyEvents {
  public:
  	virtual void ready() = 0;                    // at most once, never inside the adapter's make or open
  	virtual void failed(std::string detail) = 0; // at most once, before ready: the node fails start-failed, `detail`
  protected:
  	~ReadyEvents() = default;
  };
  ```
  `RunLoop`, `RunStore` and `RunRecording` are the framework's sides of a loop, a store and a recording (their `close`,
  their deadline); a type sees only `Loop`, `Store` and a descriptor.

**Readiness**, in this order, ordered by cost and side effects so that a refusal costs nothing:

| # | What | When it fails |
|---|---|---|
| 0 | `start` returns; feeds, wakes, handoffs and kept values already exist (made at acquire: memory only, no side effects). The code is prepared and drives the block, as now (`node_host.cpp:301-306`) | a failed `start`: see "A start that fails" below |
| 1 | **Claims**, in the same main-loop turn: each is judged (6.9). A refusal fails the node now, and nothing below is touched. A wait holds every later step. Once all are granted, ports are probed (main thread, a non-blocking bind) | `start-failed: <claim's text>` |
| 2 | **Stores and recordings**, opened on the I/O executor, one job each, in request order (5 s deadline each) | `start-failed: <file>: <why>`, `start-failed: cannot open <path>: <why>` |
| 3 | **Loops**, once step 2 is ready: dedicated threads started, strands registered. Threads last, so a refused or failed store starts none | `start-failed: cannot start loop <name>: <errno text>` |
| 3' | **Outside nodes**, once the block is ready and step 1 is granted; in parallel with steps 2 and 3, since they wait on the engine, not on the disk | `engine-refused: <detail>` (as now) |
| 4 | **`build`**, once 3 and 3' are ready. Then the handoff check (6.6), then the staged loop sources are released (6.4) | `build`'s own `FailureReason` |

**A start that fails** (`start` returns a failure): nothing was judged, opened or started. The handoffs, feeds, wakes,
kept values and recording requests made so far are freed at once, on the main loop, as handoffs and recordings are
today (`node_host.cpp:288-298`); the block was never driven by this run's code, so nothing can reach them.

**A `build` that fails**: the run is torn down by I5 as any run (6.10): its loops are running, with nothing armed
(staged sources are destroyed unrun with the run object, 6.4); 4a is skipped (no run object); 4b closes them.

### 6.2 Kept values and the mailbox

**Purpose.** State the code and the run object share, made before either exists. A mailbox is the common case.

```cpp
// ear/core/mailbox.hpp: sendspin's mailbox.hpp, moved verbatim into ear::
/// One value from one writing thread to one reading thread, the newest only: a seqlock. The writer never waits and may
/// be the realtime thread; the reader retries and must not be.
template <class Value> class Mailbox { public: void publish(const Value&) noexcept; std::optional<Value> read() const noexcept; };
```

- **API.** `T& cx.keep<T>(args...)` in `start`, `T& ready.kept<T>()` in `build`: constructed in place on the main loop,
  held by the run as a `std::unique_ptr<KeptValue>` (`struct KeptValue { virtual ~KeptValue() = default; }`, with
  `Kept<T>` holding the value). One per type `T` per run.
- **What `T` may be.** `keep` static-asserts that `T` is not a `RunObject`, a `HandoffConsumer`, a `std::thread` or a
  `std::jthread`: a kept value is shared memory, not a second run object. `T`'s destructor runs on the main loop at
  step 5 and must not block or join (documented; a grep cannot see inside every `T`).
- **Threads.** Whatever `T` promises. The framework promises only its address: stable from `keep` to step 5.
- **Lifetime.** Freed at step 5 after the run object, in reverse order of `keep`, before feeds and wakes (6.10);
  leaked with a leaked run; freed at once with a failed `start`.
- **Harness.** Real: a kept value is plain memory.
- **Replaces.** sendspin's `Mailbox` members of the worker (`worker.cpp:360-361`), which the code reaches today only
  because the run object is built in `start`.
- **Why not a resource kind** (the framework page's 7.3 row): a mailbox has no adapter, no readiness, no loss and no
  teardown action. A kind would add a variant member and an adapter for a constructor call. Gate G2.

### 6.3 Wake

**Purpose.** The realtime thread (or any thread) tells a loop "look now", at most once until the loop answers.

```cpp
namespace wake {
/// A wake of a type, by name (as a bank's, checked at compile time). Not listed in the spec: wakes cost no thread and
/// no file, so a run asks for the ones it uses.
template <spec_detail::BankName Name> inline constexpr WakeHandle<Name> named{};
}
class Wake {
public:
	/// Any thread, the realtime one included. Never blocks, locks or allocates:
	///   if (!pending_.exchange(true, std::memory_order_acq_rel)) write(efd_, 1);
	virtual void ring() noexcept = 0;
protected:
	~Wake() = default;
};
class WakeAdapter {  // ear_runtime: EventWakes
public:
	virtual std::unique_ptr<Wake> make(const NodeName& node, std::string_view name) = 0;   // main thread; std::system_error
protected:
	~WakeAdapter() = default;
};
```

- **Answered** by the loop that watches it (`Loop::onWake`, 6.4), in this order: `read(efd)` (draining it), then
  `pending.store(false, std::memory_order_release)`, then the handler. A ring that lands after the drain and before the
  clear finds `pending` still true and writes nothing, but the handler that follows sees its data; a ring after the
  clear writes the eventfd again. So no ring is lost and `pending` is never true with the eventfd empty for good. One
  watcher per wake (`std::logic_error` for a second).
- **Rung before it is watched.** The code drives the block before `build` arms `onWake`, so a wake may be pending when
  its watch is armed. Arming a watch on a pending wake runs the handler once, on the loop's first turn after the staged
  sources are released.
- **Realtime.** One `write(2)` on a non-blocking eventfd at most, as the handoff's wake already does from `process`
  (`handoffs.cpp:54` through `pw_loop_signal_event`) [V]. sendspin rings every cycle with data today
  (`sendspin_code.cpp:43`), about 190 writes a second at 48 kHz and 256 frames; gated, it writes once per notifier turn.
- **Lifetime.** Made at acquire; its watch is cancelled with its source (or at 4b); freed at step 5.
- **Harness.** A flag; `harness.ring(played)` or the code's own ring marks it, and the watching loop runs the handler on
  its next turn.
- **Tests.** The order above, on the harness (a ring between drain and clear, by a hook) and on real threads (a stress
  test: a ringing thread and a watching loop for 10^6 rings, every ring's data seen, the loop never stuck).
- **Replaces.** `EventFd` (`event_fd.hpp`) as the bell.

### 6.4 Loops

**Purpose.** A serial executor for a run's work off the realtime thread and the main loop (Q5: strands by default,
dedicated on request).

```cpp
// ear/core/loop.hpp
namespace loop {
/// A loop of a type, declared in its spec. Name: 1 to 8 of [a-z0-9-], unique in the spec (both checked at compile time).
template <spec_detail::LoopName Name> inline constexpr LoopHandle<Name, LoopKind::strand> strand{};
template <spec_detail::LoopName Name> inline constexpr LoopHandle<Name, LoopKind::dedicated> dedicated{};
}
template <auto... Handles> struct Loops;   // in TypeSpec's `.loops` (6.12)

/// A timer or a watch. Destroying it cancels it: its handler never runs after the destructor returns. Destroyed on its
/// loop's thread; or on the main loop inside build (a staged source); or on any thread once its loop is closed (4b).
/// Anywhere else is a type bug (asserted in debug builds and in the harness).
class LoopSource { public: virtual ~LoopSource() = default; };
class Loop : public Executor {
public:
	// Executor::post (executor.hpp): any thread; never runs `work` inside the call; serial, in post order. This is the
	// run's own intake: accepted until 4b. Work posted after 4b is destroyed unrun, on the caller's thread.
	/// The loop's thread, or build (staged). Once, `delay` from now.
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

- **Staging.** Everything `build` sends to a loop (sources armed, work posted) is staged on the main loop, not handed
  to the loop's thread. When `build` returns a run object, the framework releases the staged items to their loops, in
  the order they were made. When `build` fails, the staged items are destroyed unrun with the run object. So no handler
  ever runs against a run object that is not built yet, or that a failed `build` already destroyed.
- **The source contract.** Before a run object calls `closed`, it has destroyed every source it armed (on the source's
  loop, as sendspin's `close` does above). The framework counts live sources per loop. At 4b a source still armed is a
  type bug: the loop cancels it itself (so its handler never runs again), the harness fails the test, debug builds
  assert, and a release build says `node <n> ear.loop-source-left: <k> sources were still armed on loop <name> when the
  run closed` and goes on. The run object's member that still holds such a source is destroyed at step 5, on the main
  loop, after the loop closed, which the destruction rule allows.
- **Two intakes.** From step 1 a loop refuses work from outside the run (`closeToOthers`): slice 5's actions use
  `postFromOutside`, so a queued call is answered `restarted` or `stopping` at step 1, and one already running finishes
  and replies before the goodbye that 4a posts behind it. The run's own posts and sources (its goodbye among them) are
  accepted until 4b. So control-plane.md 5.5 and 7.6 answer a queued call, and let a running one reply, at one defined
  moment: step 1.
- **Threads and realtime.** A handler runs on the loop's thread, one at a time per loop. A strand runs on a framework
  pool thread (`min(4, hardware threads)` to start); a dedicated loop has its own thread, named `ear-<name> <node>`,
  cut to 15 bytes. A strand has no thread of its own and no thread name; its name appears in diagnostics. Nothing here is
  realtime. A dedicated loop may block (a library's poll, a disk write, a feed write with patience); a strand must not:
  debug builds assert at every blocking primitive (`FeedWriter::write` and `idle` with patience, `Store::replace`) that
  the caller is not a strand.
- **A slow strand.** A strand handler running past 100 ms is an `ear.loop-slow` diagnostic (`node <n> ear.loop-slow:
  loop <name> ran one handler for <ms> ms`). Past 1 s the pool starts a replacement thread (at most 16 in all), so one
  stuck strand delays no other node; the stuck thread rejoins the pool when its handler returns. A dedicated loop is
  never judged.
- **Lifetime.** Started at readiness step 3 (6.1); closed to others at step 1; closed at 4b, after the run object's
  `closed`, so its goodbye may still post to its loops. Their memory is freed at step 5, after the run object: their
  handlers point into it. A dedicated loop's thread is joined at step 5, after it posted that it exited, so the join
  returns at once (as `~SendspinWorker` joins a finished thread today, `worker.hpp:27`). A loop of a run that never got
  as far as step 3 is never made.
- **What a loop is not.** No priority, no affinity, no realtime scheduling; no futures; no cross-run sharing (a loop is
  one run's). A type that wants the 10 ms poll asks for it (`every(10ms)`); the framework does not poll for it.
- **Harness.** Manual: each loop is a queue the test turns, on a fake clock (section 10).
- **Replaces.** `SendspinWorker`'s two `std::thread`s (`worker.cpp:93`, `200`), its stop `EventFd`, `waitForStop`, the
  10 ms `poll()` loop (`worker.cpp:278-299`) and the notifier's (`309-323`). `ConnectionWatch` stays the type's: a pure
  judge the tick feeds.

### 6.5 Feed

**Purpose.** Audio from an ordinary thread to the realtime thread, the handoff's reverse (Q7's paced feed of slice 9
builds on it).

```cpp
class FeedReader {   // the run's code, realtime
public:
	/// Planar, zeros after what it holds. Returns frames read.
	virtual std::uint32_t read(std::span<const std::span<float>> channels, std::uint32_t frames) noexcept = 0;
	virtual std::uint32_t skip(std::uint32_t frames) noexcept = 0;   // discards; returns how many
	virtual std::uint32_t capacityFrames() const noexcept = 0;
protected:
	~FeedReader() = default;
};
class FeedWriter {
public:
	/// One writing thread: a dedicated loop or a thread a library owns, never a strand (asserted). Up to `frames` frames
	/// from `interleaved`; with no room waits up to `patience`. Returns frames accepted. Closed: see below.
	virtual std::uint32_t write(std::span<const float> interleaved, std::uint32_t frames,
	                            std::chrono::milliseconds patience) = 0;
	/// The same thread: waits `patience`, accepting nothing. For a writer that has nothing to give but must not spin
	/// (sendspin's library thread on a format it did not ask for). Closed: see below.
	virtual void idle(std::chrono::milliseconds patience) = 0;
	/// Any ordinary thread. What was written so far is gone at the next read.
	virtual void clear() noexcept = 0;
	virtual bool closed() const noexcept = 0;
	virtual std::uint32_t capacityFrames() const noexcept = 0;
protected:
	~FeedWriter() = default;
};
```

`StartOf::feed(bank, n)` returns the reader; `ReadyOf::feed(bank)` returns the writer. The bank is an output bank of the
spec, routable or inside, checked at compile time; one channel per port of it.

- **Closing, the contract.** The framework closes the feed at I5 step 1. A `write` or `idle` already waiting when the
  close comes wakes and returns (0 frames) at once, so a writer blocked on a long patience releases its thread for the
  goodbye. A `write` or `idle` that *begins* after the close accepts nothing and waits out its whole patience before it
  returns 0. So a library thread that calls again and again (sendspin-cpp's sync thread calls `on_audio_write` with
  20 ms, `sendspin-src/src/sync_task.cpp:56-58`, `transfer_buffer.cpp:53` [V]) sleeps between calls from step 1 until
  4a stops it, whether a stream was asked for or not, and never spins. The cost: a type's own patience bounds how long
  its writer can delay its own goodbye after the close (sendspin's: 20 ms).
- **Implementation.** sendspin's `feed.cpp` (`FrameRing`, the writer's flag and fences, the room eventfd,
  `feed.cpp:45-69`) moves into `ear_runtime` as `RingFeeds`, with `close()` (sets `closed`, rings the room eventfd)
  and `idle`. The closed branch's wait is a `poll` on an eventfd nothing writes after the close, so it sleeps.
- **Latency.** The feed declares `[0, capacity]` on its bank itself, as `declareLatency` would; a type that also
  declares that bank gets the existing `std::logic_error` ("declared twice", `node_host.cpp:120-121`).
- **Counters.** It counts frames a read came up short while a writer had written before (`shortfall`), for slice 5's
  sampled counters; nothing reads it in slice 3.
- **Lifetime.** Made at acquire; closed at step 1; freed at step 5.
- **Harness.** A real `FrameRing` with a writer that never waits (patience ignored, returns what fits; `idle` returns
  at once), so a test on one thread cannot hang.
- **Replaces.** `Feed` (`feed.hpp`, `feed.cpp`), sendspin's `declareLatency("from", ...)` (`sendspin_type.cpp:102`), and
  the stop-eventfd wait in `on_audio_write` (`worker.cpp:125-131`) [V].

### 6.6 Handoff, with a late consumer

**Purpose.** As today (`node_type.hpp:146-161`), plus: a consumer that is part of the run object, which does not exist
yet when `start` asks for the handoff.

```cpp
class HandoffConsumer {   // node_type.hpp, amended
public:
	virtual ~HandoffConsumer() = default;
	/// The handoff's thread, once, before the first consume(): the consumer is attached, and `skippedFrames` frames the
	/// writer wrote before that were drained and discarded. A consumer that numbers frames starts at skippedFrames.
	virtual void attached(std::uint64_t skippedFrames) {}
	virtual void consume(std::span<const float> interleaved, std::uint32_t frames) = 0;   // unchanged
	virtual void finish() = 0;                                                             // unchanged
};
```

- `cx.handoff(bank, n)` makes a handoff with no consumer; `ready.attach(bank, consumer)` gives it one, once. Until
  then the handoff's thread drains the ring and discards, counting the frames; they are not dropped frames (the writer
  accepted them) and are not added to the node's `dropped=`.
- **Publication.** The consumer is an `std::atomic<HandoffConsumer*>`, stored with `memory_order_release` by `attach` on
  the main thread, which then rings the handoff's wake. The handoff thread loads it with `memory_order_acquire` at the
  start of each drain; the first drain that sees it calls `attached(skipped)`, then hands over everything from that
  point. A drain already in progress when `attach` lands discards its batch and is counted in `skipped`. This replaces
  the plain reference held today (`handoffs.cpp:24-25`, `104`) [V].
- **Why `attached` carries the count.** sendspin dates captured audio by frame index: its code publishes
  `CaptureAnchor{framesWritten_, startRawNs}` and adds what `write` accepted (`sendspin_code.cpp:25-26`), and
  `ToServer::consume` numbers frames from its own `consumed_` (`to_server.cpp:20-36`) [V]. Discarded frames advanced the
  first count and not the second, so every capture stamp would be early by the discarded audio. `ToServer::attached`
  sets `consumed_ = skippedFrames`, and the two counts agree again.
- **Checks.** After `build` returns, a handoff that has no consumer throws `std::logic_error` ("node <n>: the handoff
  on bank <b> was never attached"), a type bug the harness catches. `attach` twice, or on a bank with no handoff, throws
  too. At most one handoff per bank in a run (`std::logic_error`), so a bank names it.
- **Teardown.** At step 3 a handoff with a consumer calls `attached` first if it has not yet, then drains to it, then
  calls `finish`; a handoff with none (its run was torn down before `build`) drains, discards and calls nothing. The
  consumer must live until the handoff closed (step 3); the run object does, since it closes at 4a.
- **A type that only takes audio out.** `RunCode::toHandoff(bank)` is a framework code that writes the bank into its
  handoff each cycle and writes nothing else. It takes a bank handle and is refused at compile time for an output bank.
  `passThrough` keeps its rule of one pair or more (`pass_through.cpp:48`) [V]: a pass-through never writes a handoff,
  and a type's own code writes its handoff itself (`writer.write(io[bank])`), as sendspin's does, since it publishes a
  capture anchor with each write.
- **Harness.** As today: writes go straight to the consumer once attached; before `attach` they are discarded and
  counted, and `attached(n)` is called with that count.
- **Tests.** Frames written before `attach`, then a capture stamp checked after it (sendspin's `type_test.cpp`); a
  handoff never attached throws after `build`; the release/acquire publication under a writer at full rate (runtime).
- **Replaces.** Nothing in sendspin directly; it is what lets `ToServer` live in the run object without being built in
  `start` (today `sendspin_type.cpp:100`), and what lets `file` open its WAV on the handoff's thread (6.11).

### 6.7 The I/O executor

**Purpose.** The framework's one thread for disk work, so a slow disk delays no node's loop and never the main loop
(the framework page, 7.3).

```cpp
class IoExecutor : public Executor {   // ear_core interface; ear_runtime: IoThread, one thread, made at boot
protected:
	~IoExecutor() = default;
};
```

- Types never see it. In slice 3 it opens stores and recordings and runs every store write; slice 8's `/proc` reads and
  slice 9's runtime files will use it.
- **Jobs hold shared state.** Every job holds a `shared_ptr` to the state it fills (the store's text and mode, the
  recording's descriptor), never a `RunStore`, `RunRecording` or `ReadyEvents` pointer. Its completion is posted to the
  main loop and checked there against the run's generation and the object's liveness: a completion for a run that is
  gone, or past its deadline, is dropped, and a descriptor it opened is closed by a second I/O job. So a run torn down
  while its open is in flight, or an open past its 5 s deadline, frees nothing a job will touch.
- **One thread**: a hung disk stalls every store's open, every recording's open and every store write (section 16).
  Each has a 5 s deadline, so a node fails visibly instead of starting forever.
- **Stopping.** `main.cpp` asks it to stop after `NodeHost::stopAll` completed and the keeper's thread was joined:
  queued jobs are destroyed unrun, and the job in flight gets 2 s. If it is still busy then, ear writes a `log` record
  (`source` `ear`, `warn`: `the I/O thread was still busy at exit: <job>`) and ends with `std::_Exit(0)` once the output
  thread's queue is written out, as it does for a leaked run (`main.cpp:312-315`) [V]. `ear.conf` is written by then.

### 6.8 Stores

**Purpose.** A file that belongs to one node and survives its restarts: named by the framework, kept narrow, read and
trusted before the run starts, replaced only if unchanged, swept at boot.

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
/// One store of a type, as a compile-time constant: its suffix (as a bank's name) and its rule. Refused at compile time:
/// maxBytes 0 (left out), maxBytes above 1 MiB, a mode with bits outside 0777.
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
	/// if the file still holds text(). Waits at most 5 s for that. Error: a sentence naming the file ("<file>: changed by
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

- **Writes are serialised.** Every write of a store, `replace` or `replaceLater`, is one I/O job, so writes of one store
  run in the order they were made, each against the text the one before committed. Two writers in flight cannot both
  compare against one base, so "changed by something else" means something outside ear.
- **Names.** `<type>-<node>.<suffix>`, the framework's (I9): sendspin's is `sendspin-<node>.key`, today's name, so no
  file moves. The staging name stays `ConfinedFolder`'s (`.<name>.ear-staging`, `confined_folder.cpp:239`) [V].
- **Folder.** The graph file folder, as today. The keeper's watch ignores other names (sendspin.md section 8) [V there].
- **Open** runs at readiness step 2, after the store's claim was granted (6.9), never in `start` or `build`, so no run
  object reads a file. The failures are today's sendspin texts, now the framework's, so the system tests keep their
  lines (`sendspin_pairing_test.cpp:206`: `node ma failed start-failed: sendspin-ma.key: malformed: not JSON`) [V]: the
  framework writes `<file>: <why>` and sendspin's `distrust` returns `malformed: not JSON`.
- **Mode.** Narrowed at the boot sweep, at every open (before the text is judged: a refused file may hold a private
  key), and before every write (a write stages with the mode of what it replaces). These are today's three narrowings
  (`key_store.cpp:51-62`, `75-76`, `121`) [V], now one rule for every store.
- **Never a new identity over distrusted text.** A file `distrust` refuses is left as it was and fails the node; only a
  missing file is created.
- **Boot sweep findings.** A `log` record at `warn` with `source` the type's name, as today (AGENTS.md: "A sendspin key
  file it cannot narrow at boot is a log record from sendspin"). Slice 5 may make it a diagnostic; this page does not.
- **Exclusive.** Its implicit claim, `store:<file name>` (6.9), is judged before the open. One run per node name at a
  time (I7 and today's `olderGoing`, `node_host.cpp:64-69`) already makes a store one run's; the claim covers the leaked
  run that I7 does not.
- **Lifetime.** Open at readiness (deadline 5 s: `<file>: not opened within 5 s`); usable until 4b; freed at step 5,
  after the loops (a write in flight holds the store's shared state, 6.7, and its `done` is dropped).
- **Harness.** In memory, by handle: the test presets a store's text, makes an open fail, or makes the next write fail;
  it reads what was written (section 10).
- **Replaces.** `NodeKeyStore::open` (`key_store.cpp:64-97`), `narrowKeyFiles` (`51-62`), its call and boot log
  (`sendspin_type.cpp:137-144`), `NodeFolders` entirely. `NodeKeyStore` keeps only the library's provider interface
  (`load_blob`, `save_blob`) over the parsed record, writing with `Store::replace` from the worker or a library thread,
  which may block.

### 6.9 Claims

**Purpose.** Something ear-wide that one run at a time may hold (I8): a listening TCP port, a UDP port, and, implicitly,
a store's file and a recording's path.

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
	/// UDP: all addresses with neither SO_REUSEADDR nor SO_REUSEPORT. UDP has no TIME_WAIT, and PipeWire's rtp-source
	/// binds with SO_REUSEADDR (pipewire-1.4.2/src/modules/module-rtp-source.c:252, bind at :309) [V], so a probe that
	/// set it too would pass while a receiver holds the port.
	virtual std::expected<void, std::string> udp(std::uint16_t port) = 0;
protected:
	~PortProbe() = default;
};
```

The registry is core's (`ClaimRegistry`, inside `NodeHost`, main loop only), keyed by kind and value: `tcp:8928`,
`udp:5006`, `store:sendspin-ma.key`, `recording:take1.wav`. A recording's key is its path with `.` and empty components
removed (`./a//take1.wav` is `recording:a/take1.wav`); `..` and absolute paths are already refused by `file`'s check.
Who claims:

| Claim | Made by |
|---|---|
| `tcp:<p>` | `cx.claim(claim::TcpListen{p})` (sendspin listening) |
| `udp:<p>` | `cx.claim(claim::Udp{p})`, and implicitly every `RtpReceiverRequest` (the `rtp` type) |
| `store:<file>` | implicitly, every store of the spec, for every run |
| `recording:<path>` | implicitly, every `cx.recording(...)` |

At readiness step 1 (6.1) each claim of a run is judged against its holder, if any:

| Holder | Outcome | Detail (when it fails) |
|---|---|---|
| none | granted, then probed (ports only) | the kernel's reason |
| a running node, or one starting that is not going | refused at once | `held by node <n>` |
| a node whose run failed (a loss, `lost`) and was not torn down: it keeps its resources until an update (`node_host.cpp:375-377`) | refused at once | `held by node <n>, which failed` |
| a run that is going (a restart's old run is never one, by I7; a removed or renamed node still closing is) | waits: the node stays `starting`, and `ear.claim-wait` says why; granted when step 5 releases it, then probed | after 15 s: `held by node <n>, which did not let go within 15 s` |
| a leaked run (I5a), port | abandoned: probed afresh; granted if it passes | the kernel's reason; G3 below |
| a leaked run, store or recording | refused | `held by a run of node <n> that did not close`; G3 below |

- **The wait, visible.** When a wait begins, the framework says `node <b> ear.claim-wait: <key> held by node <a>,
  which is closing` (a `diagnostic` record, in `output-records.json` from S3.6). The holder's own close deadline (10 s)
  passes before the waiter's (15 s), so a wait ends in a grant, a release by a leak's abandonment (ports), or a refusal
  that names the leaked run.
- **A node refused by a leaked run starts by itself when that run closes late** (gate G3, recommended (a)). The
  refusal's detail says so: `held by a run of node <n> that did not close; it starts when that run closes`. The leaked
  run's late close (6.10) releases its claims and then starts again, once, every node still failed by a refusal that
  named it, as an update with the same settings would if it restarted anything. A port whose fresh probe failed while a
  leaked run held it is treated the same.
- **Release.** At step 5, last. A waiter is granted in arrival order. A waiter of a run torn down before its claims were
  granted leaves the queue at step 1.
- **The probe's limit stays:** a program taking the port just after the probe is not caught; sendspin's library retries
  every 5 s (sendspin.md section 10) [V there], and an `rtp` receiver logs PipeWire's bind failure as today.
- **mDNS names are not claimed.** sendspin's instance name is `ear-<node>-<first 8 of client id>` (AGENTS.md), and the
  client id comes from the node's own key file, so two nodes cannot advertise one name. The framework page's I8 listed
  them; this page drops them.
- **Harness.** Granted by default; `harness.holdClaim(claim::TcpListen{8928}, "porch")` refuses one,
  `harness.waitClaim(...)` makes one wait and `harness.releaseClaim(...)` ends the wait, `harness.failProbe(...)` makes a
  probe fail.
- **Replaces.** `PortClaims`, `HeldPort`, `probeListenPort` (`port_claims.*`, `sendspin_type.cpp:36-47`, `83-92`), and
  their type-level scope: a claim now spans types (an `airplay` node on 7000 against anything else on 7000, an `rtp`
  receiver against an `rtpsend` sender on one port).

### 6.10 Teardown, with every primitive

I5 as amended (changes in bold). Every step that waits on another thread is asynchronous; the main loop never joins a
thread that has not already said it is done.

| Step | What | Threads it waits on |
|---|---|---|
| 1 | The barrier (`drive(nullptr)`, or the block destroyed), `release`; **every feed closed** (6.5); **every loop closed to others** (queued actions answered); **if the run is not ready yet: its claim waits leave the queue, its pending store and recording opens are orphaned (6.7), and the readiness steps it had not reached never start** | the data loop, briefly (blocking invoke, as now) |
| 2 | Outside nodes destroyed | none |
| 3 | Handoffs closed: each calls `attached` if needed, drains to its consumer (if attached) and calls `finish` on its own thread, then reports | handoff threads |
| **4a** | The run object's `close(closed)`, if `build` made one: its goodbye may post to its loops; it stops threads its library owns; it destroys every source it armed; then `closed` from any thread | the run object's loops and threads |
| **4b** | **Each loop closed**: no new work, sources still armed cancelled and counted (`ear.loop-source-left`), queued work destroyed unrun on the loop's thread, an in-flight handler finishes; each reports | loop threads |
| 5 | On the main loop: the code; the run object; the recipe; **kept values, in reverse order of `keep`; feeds; wakes; loops (a dedicated thread joined, already exited); stores; recordings closed; claims released (waiters granted)**; the config | none |

**A run torn down before it was ready** (an update, a removal or a stop while it waits on a claim, an open or an outside
node): step 1 as above; steps 2 and 3 as for any run; 4a is skipped (no run object); 4b closes whatever loops step 3 of
readiness had started, with nothing armed; step 5 frees what exists.

**The close deadline and the leaked run.** I5a's deadline (default 10 s) covers steps 3, 4a and 4b together, as slice 1
built for 3 and 4. A run past it is leaked: everything step 5 would free is kept (code, run object, kept values, feeds,
wakes, loops, stores, recordings, handoffs), an `ear.close-overdue` diagnostic names what was still open (`<n> handoffs
open, run object closing, <k> loops open`), its port claims are abandoned and its store and recording claims stay (6.9),
and the node moves on. Unlike slice 1 (`node_host.cpp:447-464`, where the detached `Channel` drops every later report,
`node_host.cpp:191-221`), the leaked run keeps listening:

- Its `Channel` is re-pointed at a **leaked-run record** that `NodeHost` keeps (in a list, never freed), instead of
  being detached to nothing. Every later handoff `closed` and run-object `closed` lands there, still in order on the main
  loop.
- The record carries on where the run stopped: a run leaked in step 3 is told to close (4a) once its last handoff
  reports, then its loops are closed (4b); a run leaked in 4a has its loops closed once `closed` comes.
- When everything it held has reported, its claims are released (waiters granted), `node <n> ear.close-late: the run
  leaked at the close deadline closed after <s> s; its claims are released` is said, and every node its claims refused
  is started again (6.9, G3). Its memory stays leaked: a late report proves the threads are done, but the record is the
  only thing that knows it, and keeping the rule "a leaked run is never freed" keeps it simple.
- `NodeHost::leakedRuns()` counts leaked runs still open; one that closed late no longer counts, so a stop after it ends
  normally rather than with `std::_Exit` (`main.cpp:312-315`) [V].
- A run whose threads never report (a library that hangs in `stop` for good) stays leaked and open until ear exits, and
  the nodes it refused stay failed with a detail that says why.

### 6.11 Recordings, deferred

**Purpose.** A file beneath the recordings folder that one run writes (slice 2's kind, `node_type.hpp:162-168`), now
opened after its claim and off the main loop.

```cpp
struct RecordingRequest { std::string path; };   // unchanged: relative to the recordings folder
// start:  cx.recording({path});                  claims recording:<normalised path>; opens nothing
// build:  int fd = ready.recording(path);        the descriptor, open, created or truncated
class RecordingAdapter {   // node_host.hpp, amended; main.cpp's ConfinedRecordings over ear_folder
public:
	/// Main thread. Creates or truncates `path` beneath the folder on the I/O executor (openat2, RESOLVE_BENEATH, as
	/// today); `events.ready()` with the descriptor in the run's shared state, or `events.failed(<why>)`.
	virtual std::unique_ptr<RunRecording> open(const NodeName& node, const std::string& path, ReadyEvents& events) = 0;
protected:
	~RecordingAdapter() = default;
};
```

- **Order.** Asked for in `start`, judged as a claim at readiness step 1 (6.9), opened at step 2, reached in `build`.
  So a path held by a running node is refused before anything is truncated, and a path held by a node still closing
  waits, as I8 asks: removing `rec` and adding `rec2` on `take1.wav` in one plan works.
- **Failure text.** The framework writes `cannot open <path>: <why>`, today's text from `file.cpp:70-72`, so the system
  tests keep their lines; a claim refusal is `cannot open <path>: held by node <n>`.
- **`file` on it.** `start` asks for the recording and the handoff and returns its `Recorder` code. `build` makes the
  run object around the descriptor and attaches its `WavWriter`, whose `attached` calls `sf_open_fd` on the handoff's
  thread (today `file.cpp:76`, on the main loop) and reports a failure with `handle.lost(...)` (section 8). Frames the
  code wrote before the attach are discarded and counted (visible change 4).
- **Lifetime.** Opened at readiness (deadline 5 s); closed at step 5, after stores and before claims are released, so
  the next run's open of that path follows the close; leaked with a leaked run, its claim kept (6.9).
- **Harness.** A real descriptor on a temporary file, or a failure the test sets (section 10).
- **Replaces.** The synchronous open in `acquireResource` (`node_host.cpp:137-141`) and `file`'s main-loop
  `sf_open_fd`.

### 6.12 `TypeSpec`, whole: the one home of its members

Both pages' members, in their final order. Every template parameter after `BankList` defaults to an empty list, so a
spec gives only what it uses, in this order (designated initialisers follow declaration order).

```cpp
// ear/core/type_spec.hpp
template <class Settings, class BankList, class StoreList = Stores<>, class LoopList = Loops<>,
          class Status = StatusTable<>, class Events = EventTable<>, class Actions = ActionTable<>,
          class Logs = LogCodes<>>
struct TypeSpec {
	Settings settings;      // slice 2
	BankList banks;         // slice 2
	StoreList stores{};     // slice 3 (6.8)
	LoopList loops{};       // slice 3 (6.4)
	Status status{};        // slice 5 (control-plane.md 5.2)
	Events events{};        // slice 5 (5.3)
	Actions actions{};      // slice 5 (5.5)
	Logs logCodes{};        // slice 5 (5.3)
};
```

Slice 3 builds the first four; slice 5 appends the last four in this order. control-plane.md 5.3 cites this one and
restates no order. `Stores`, `Loops` and `Banks` check their names' uniqueness at compile time; a store, a loop and a bank
may share a name, since each lives in its own list. Whether GCC 14 deduces the defaulted parameters for every subset
spelling is U1.

## 7. Threads, as one contract (the framework page's 7.4, completed)

| What the type writes | Runs on | May |
|---|---|---|
| Rows' parse functions, `check`, `ports`, a store's `initial` and `distrust` | any thread, pure | nothing else (`initial` may read the clock and a random source) |
| `start` | the main loop | acquire, keep, claim, ask for recordings, construct the code. No I/O, no threads, no blocking |
| `build` and the run object's constructor | the main loop | read `Store::text()`, the recording's descriptor (not write it), kept values; arm loop sources and post (staged); `attach`. No I/O, no threads, no blocking |
| `prepare`, `release` | the main loop, never with `process` | allocate |
| `process` | the realtime thread | `io`, `FeedReader`, `RtWriter`, `Mailbox::publish`, `Wake::ring` only |
| Strand handlers | a pool thread, serially per strand | anything that does not block: `post`, `replaceLater`, `RunHandle`, non-blocking sockets |
| Dedicated loop handlers | the loop's thread | also block: `FeedWriter::write` and `idle` with patience, `Store::replace`, a library's poll |
| Threads a library owns (sendspin-cpp's) | their own | what the library does, `FeedWriter::write` and `idle`, `Store::replace`; the run object stops them in `close` |
| `HandoffConsumer::attached`, `consume`, `finish` | the handoff's thread | block on I/O; `attached` opens what the consumer writes to (a socket, `file`'s WAV) |

A guard joins `standard_output_guard.cmake` as its own script, `thread_guard.cmake`: no non-test file under
`src/nodes/` (it skips every path with a `/tests/` component, since sendspin's tests use `std::thread`,
`code_test.cpp:272`, `feed_test.cpp:80` [V]) names `std::thread`, `std::jthread`, `std::async`, `pthread_create` or
`fork`. Its own test covers each spelling, a look-alike it lets through (`my_thread_count`), and the `/tests/` exclusion.
A type gets threads only as loops, and a library's own threads stay inside the library. After slice 3 sendspin's own
code passes it.

## 8. Invariants and failure modes

Invariants this slice adds, each a test's to prove:

- **P1. Nothing a run's code reaches is owned by its run object.** Code reaches only acquired resources and kept values,
  which step 5 frees after the run object.
- **P2. No type code starts a thread, and no type code does I/O on the main loop.** Loops are the framework's; stores and
  recordings open on the I/O executor; `build` gets them open; a consumer opens what it writes on the handoff's thread.
- **P3. Every loop of a run is closed before its run object is freed**, every handler is done or destroyed unrun, and no
  handler runs before `build` returned or after the run object called `closed` (staging, the source contract).
- **P4. A claim has one holder ear-wide**, across types, judged before anything of the run is opened or started; a
  leaked run's claim on something no probe can check is never handed on before that run's late close.
- **P5. A store's file is never wider than its mode** after boot, an open or a write, and never replaced by a new
  identity when its text is distrusted.

| Failure | What the operator sees | Why it is safe |
|---|---|---|
| A store cannot open: a symlink, a directory, more than `maxBytes`, unreadable, distrusted, cannot be created | `node <n> starting`, then `failed start-failed: <file>: <why>`; the file unchanged | no run object was built, no loop started; nothing read the file but the framework |
| A store or recording open hangs (a hung disk) | after 5 s, `failed start-failed: <file>: not opened within 5 s` (a recording: `cannot open <path>: not opened within 5 s`) | the job holds only shared state; its late completion is dropped (6.7) |
| A later store write is refused | sendspin: `says key-write-failed <detail>` (today's note, its text from the store) | the file keeps the other writer's text; this run's state stays in memory (the library's contract) |
| A claim held by another running node | `failed start-failed: cannot listen on port <p>: held by node <n>` (today's text, `sendspin_connections_test.cpp:235`) [V]; `cannot open take1.wav: held by node <n>` for a path | refused at step 1: no file opened, no thread started, no socket |
| A claim held by a node whose run failed | `... held by node <n>, which failed` | as above |
| A claim held by a node still closing | `starting`, `ear.claim-wait: <key> held by node <n>, which is closing`, then `running`; after 15 s, failed with `which did not let go within 15 s` | I8; the holder's close deadline (10 s) passes first |
| A port another program holds | `failed start-failed: cannot listen on port <p>: Address already in use` (today's text, `sendspin_connections_test.cpp:202`) [V]; for `rtp`, `cannot bind UDP port <p>: Address already in use` | the probe |
| A claim held by a leaked run | port: probed afresh, then granted or refused with the kernel's reason; store or path: `held by a run of node <n> that did not close; it starts when that run closes`; later `ear.close-late` and the node starting by itself | the leaked thread may still write the file |
| A strand handler that never returns | `ear.loop-slow` after 100 ms; a replacement pool thread after 1 s; at teardown the close deadline leaks the run | other nodes' strands keep their threads |
| A dedicated loop that never drains | at teardown, after 10 s, `ear.close-overdue ... 1 loops open`; the node moves on | the loop's memory and thread are leaked, never freed |
| A loop's thread cannot be started | `failed start-failed: cannot start loop <name>: <errno text>` | no run object was built |
| A run object that leaves a source armed | `ear.loop-source-left` (release build); the harness and debug builds fail | the loop cancels it at 4b |
| A feed writer blocked at teardown | returns 0 at step 1; later calls sleep their patience | its wait is bounded by its own patience |
| A post, ring or handle call after the run closed | dropped | loops destroy late work unrun; the handle is detached |
| `file`'s WAV cannot be started on the handoff's thread (libsndfile refuses the descriptor) | `running`, then `failed start-failed: cannot open take1.wav: <libsndfile's reason>` | `lost` from the handoff's thread, crossing to the main loop |
| The close deadline at stop | ear proceeds, as today; `main.cpp` ends with `std::_Exit` once any leaked run is still open | unchanged from slice 1, but a run that closed late no longer forces it |
| The I/O thread busy at exit | a `log` record `the I/O thread was still busy at exit: <job>`, then `std::_Exit(0)` | `ear.conf` was written first |

**Visible changes**, each with a system test in S3.6 or S3.7:

1. **A corrupt key file** now fails sendspin before it was `running`: today the worker reads the file after the recipe
   returned (`worker.cpp:194`, `node_host.cpp:307`) [V]. Now the store opens before `build`, so the node goes
   `starting` then `failed`. The system test checks only the `failed` line (`sendspin_pairing_test.cpp:206`) [V]; any
   check of `running` before it is found in step S3.7 (U4).
2. **Two `file` nodes on one path**: today both run and corrupt one file. Now the second fails `start-failed: cannot
   open take1.wav: held by node <first>`; removing one and adding the other in one plan works.
3. **An `rtp` node on a UDP port another program holds** now fails `start-failed: cannot bind UDP port 5004: Address
   already in use`, where today it stays `starting` for good (AGENTS.md; rtp.md section 9 lists the missing failure).
   Two `rtp` nodes on one port are refused `held by node <n>` (today PipeWire binds both with SO_REUSEADDR); two
   multicast groups on one port are refused too (section 16).
4. **`file` records from the first frame after its run is ready**, not from the first cycle: the frames written while
   its recording opens on the I/O executor (milliseconds) are discarded and counted in `attached`. The WAV is still
   complete and `dropped=0`.
5. **New diagnostics**: `ear.claim-wait`, `ear.close-late`, `ear.loop-slow`, `ear.loop-source-left`; `ear.close-overdue`
   gains its loop count. Each is in `output-records.json` from the step that adds it.

## 9. The carried sendspin-cpp log patch

**Today.** On a host build `SS_LOG*` is a bare `fprintf(stderr, ...)` behind one process-wide level
(`src/platform/logging.h:56-63` in the pinned tree, `7be2278`) [V]. ear sets `WARN` (`worker.cpp:218`) [V]; its lines
reach standard output as unattributed `stderr` records at `warn`, and with two sendspin nodes nobody can tell whose.
There are 533 `SS_LOG*` call sites in 36 files [V: grep of the pinned tree]; none takes a client.

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
   entry point (`start`, `loop`, `stop`, `connect_to`, the destructor, `PlayerRole::notify_audio_played`,
   `SourceRole::write_audio`), so callbacks it makes from there, the persistence provider's included, inherit it.
4. The three library threads inherit the context of the thread that starts them: `ProtocolTask::start`,
   `SyncTask::start` and `SourceTask::start` (`protocol_task.cpp:60`, `sync_task.cpp:134`, `source_task.cpp:161`) [V]
   capture the caller's context and open a `LogScope` at the thread's entry. They are started inside `client.start()`,
   which is scoped.
5. The three IXWebSocket callbacks the library registers (`client_connection.cpp:142`, `ws_server.cpp:75`, `107`) [V]
   capture the context and open a scope in the lambda. IXWebSocket's own `fprintf`s (`IXSocketServer.cpp`) stay
   unattributed `stderr` records; `ixwebsocket-limits.patch` already bounds them per connection.

**ear's side**, in sendspin's run object, on the worker: `set_log_sink(&forward)` once per process (`std::call_once`),
and `config.log_context = &handle` (the run's `RunHandle`). `forward` maps `SS_LOG_ERROR`, `WARN`, `INFO` and `DEBUG`/
`VERBOSE` to ear's four levels and calls `handle.log({.level, .source = "sendspin-cpp", .text, .topic = tag, .cut})`.
A null context (a line outside every scope) becomes an unattributed `log` record from `sendspin-cpp`, still better named
than `stderr`. Records then read:

```json
{"v":1,"seq":91,"t":"…","kind":"log","node":"kitchen","run":4,"source":"sendspin-cpp","topic":"sendspin.conn","level":"warn","text":"…","msg":"node kitchen sendspin-cpp sendspin.conn warn: …"}
```

- **Lifetime.** The context is the run handle, which outlives the run (5). The client is destroyed in the run object's
  goodbye, on the worker, before `closed`, so no thread of that client logs after the run closed.
- **Cost.** A rebase at each bump of the pin, as for the listener patch; configure refuses a tree without it
  (`carried_patch_missing`, `cmake/CarriedPatch.cmake`) [V]. `sendspin_patched_dep` takes a list of patches.
- **Not upstreamed here.** Offering it upstream is worth a try; ear keeps it carried until a release has it.

## 10. The type harness, for slice 3

`TypeHarness<spec, Behaviour>` fakes every new kind on the test's one thread, each named by the spec's handle (a
misnamed or undeclared handle does not compile, as `harness.input(card::play, 0)` does not today,
`type_harness.hpp:23-30`). `NodeHarness` under it takes names, for hand-made types.

| Kind | The fake | What a test does |
|---|---|---|
| Loop | a queue and a fake clock per loop; kind kept, so blocking asserts still judge strands | `runLoops()`; `advance(ms)`; `loop(worker)` to turn one; `armed(worker)` counts its live sources |
| Wake | a flag; its `onWake` handler runs on the watching loop's next turn | `ring(played)`, or the code rings it |
| Feed | a real `FrameRing`; the writer never waits | `feedWriter(from)` to write from the test, then `process` |
| Handoff | writes go straight to the consumer once attached; before, discarded and counted | `handoffFrames(to)`, `skipped(to)` |
| Store | in memory, by handle | `presetStore(keyFile, text)`, `failStoreOpen(keyFile, why)`, `failNextWrite(keyFile, why)`, `storeText(keyFile)` |
| Recording | a real descriptor on a temporary file, or a failure | `failRecording(path, why)`, `recordingText(path)` |
| Claim | granted | `holdClaim(claim::TcpListen{8928}, "porch")`, `waitClaim(...)`, `releaseClaim(...)`, `failProbe(...)`; `claims()` lists what start asked |
| Readable descriptor | delivered by the test | `readable(fd)`: the `onReadable` handler runs on its loop's next turn |
| Run handle | captured | `notes()` (paced as the framework paces), `logs()`, the fact |
| Kept value | real | — |

**Rules a test can rely on:**

- **Readiness.** `start()` readies everything at once (claims granted, stores and recordings open, loops made), then
  calls `build`, then releases the staged sources, all before it returns, unless the test made a claim wait or an open
  fail first. After `releaseClaim`, readiness carries on at once.
- **The clock.** `runLoops()` runs posted work, rung wakes and readable deliveries on every loop until all are idle; it
  never fires a timer. `advance(d)` moves the fake clock by `d` and fires, in time order, every timer due at or before
  the new now, `after(0)` included (so `advance(0ms)` fires it), running each loop until idle after each.
- **Store writes.** A `replace` completes inside the call; a `replaceLater` completes, and its `done` runs, on the next
  `runLoops()`.
- **`close()`** runs I5 as the framework does: release, feeds closed, loops closed to others, handoffs finished, the run
  object's `close`, then the harness turns loops until `closed` is called. **If every loop is idle and `closed` has not
  come, it fails at once** with `std::runtime_error` naming the loops and the sources still armed on each, since
  nothing can change on one thread. Then 4b (a source still armed fails the test, 6.4) and step 5. A real-time bound
  (10 s) applies only when the test chose to start real library threads.
- So a run object that starts its library from a timer (sendspin's `after(0)`) never starts it in a test that does not
  advance the clock: the all-type tests run sendspin without a network.

`notYetInHarness` and its checks in `test/nodes/harness_test.cpp` go: every type runs through the harness (gate G6).

## 11. Where the code lives

| Package | Gains | Never |
|---|---|---|
| `ear_core` | `run_handle.hpp`, `mailbox.hpp`, `loop.hpp`, the wake, feed, store, recording, claim and probe interfaces, `IoExecutor`; `ClaimRegistry` and the leaked-run record in `NodeHost`; `StartOf`, `ReadyOf`, `build`, `StoreRule`, `Stores`, `store::file`, `Loops`, `loop::strand`, `loop::dedicated`, `wake::named` and `TypeSpec`'s new members in `type_spec.hpp`; `NodeType::stores()`; `RunCode::toHandoff`; `ResourceRegistry` over a struct of adapters | threads, files, sockets (unchanged) |
| `ear_runtime` (new, `src/runtime`) | `StrandPool` and `DedicatedLoops` (one `LoopAdapter`), `EventWakes`, `RingFeeds`, `IoThread`, `KernelPortProbe`, `ConfinedStores` (store adapter and boot sweep, over `ear_folder`) | PipeWire; policy (what a claim means, when a store is trusted: core's and the type's) |
| `ear_type_harness` | the fakes of section 10 | PipeWire, real threads |
| `ear_node_sendspin` | loses about 650 lines (section 12) | `std::thread` (the guard) |
| `ear_node_file` | its WAV opened on the handoff's thread (`attached`), its run object made by `build` | I/O on the main loop |
| `ear_all_node_types` | `allNodeTypes()` takes no argument; the template's `made` goes | — |
| `ear` (`main.cpp`) | makes the `ear_runtime` adapters and the I/O executor, sweeps at boot (findings to `StatusLog` as `log` records), passes them to `ResourceRegistry`, `nodes.logTo(log)`, stops the I/O executor last (6.7). `ConfinedRecordings` becomes a recording adapter whose open runs on the I/O executor | computing anything |

Dependencies: `ear_core` <- `ear_runtime` -> `ear_folder`; `ear_runtime` <- `ear`. No node type links `ear_runtime`
(configure's `ear_refuse_node_type_links` already refuses a type linking another type, not a package; this page asks
the guard to refuse `ear_runtime` and `ear_pipewire` too, so a type reaches them only through core's interfaces).

`ResourceRegistry` takes `ResourceAdapters{.cards = &..., .receivers = ..., .handoffs = ..., .recordings = ...,
.loops = ..., .wakes = ..., .feeds = ..., .stores = ..., .probe = ..., .io = ...}`; a null adapter refuses its kind
with "no <kind> in this host" at acquire, as a missing recordings folder does today (`node_host.cpp:34-41`). Tests give
what they use, and the three- and four-adapter constructors go.

## 12. sendspin and file, file by file

sendspin's namespace becomes `ear::nodes::sendspin` (AGENTS.md, adding a type, step 3).

| File (lines today) | Fate |
|---|---|
| `include/ear/nodes/sendspin.hpp` | `makeSendspinNodeType()`, no argument; includes no `node_folders.hpp` |
| `sendspin_type.cpp` (146) | renamed `sendspin.cpp`: `Sendspin::start`, `Sendspin::build` and `makeSendspinNodeType` (about 40 lines). `SendspinConfig`, `SendspinType`, `HeldPort`, the old `SendspinRun` and the boot sweep go |
| new `sendspin_spec.hpp` | banks, `keyFile`, `worker`, `notifier`, `played`, `spec`, `Sendspin`, `feedFramesFor`, `emptyKeyRecord`, `whyNotKeyRecord` declarations |
| `settings.hpp`, `settings.cpp` (44, 67) | namespace only; the table stays named `"sendspin"` |
| `worker.hpp`, `worker.cpp` (37, 398) | become `run.hpp`, `run.cpp`: `SendspinRun : RunObject` built from `ReadyOf<spec>&`, with the library's listeners (unchanged bodies but `on_audio_write`'s idle branch), `startClient`, `tick`, `notify`, `stopClient`, `close`. Gone: both `std::thread`s, the stop `EventFd`, `waitForStop`, `poll()`, `finish`'s joins, `NoteGate`, the feed and mailbox members. About 300 lines |
| `sendspin_code.hpp`, `.cpp` (48, 46) | a spec code: `void process(Io<spec>, const GraphCycle&) noexcept` over `io[to]`, `io[from]`; holds `FeedReader&`, `Mailbox<PlayProgress>&`, `Wake&`, `RtWriter&`, `Mailbox<CaptureAnchor>&`. Same arithmetic |
| `key_store.hpp`, `.cpp` (41, 141) | `NodeKeyStore` keeps `load_blob`, `save_blob` over a parsed `KeyRecord` and a `Store&`; `whyMalformed` becomes `whyNotKeyRecord` (returning `malformed: ...`); `KeyRecord::parseTrusted`; the empty record becomes `emptyKeyRecord`. `open` and `narrowKeyFiles` go. About 70 lines |
| `feed.hpp`, `feed.cpp` (37, 72) | deleted: moved to `ear_runtime` (`RingFeeds`) |
| `mailbox.hpp` (48) | deleted: moved to `ear/core/mailbox.hpp` |
| `event_fd.hpp` (33) | deleted: `EventWakes` and the feed's room signal in `ear_runtime` own their eventfds |
| `note_gate.hpp`, `.cpp` (27, 40) | deleted: the run handle paces `say` |
| `port_claims.hpp`, `.cpp` (24, 36) | deleted: `ClaimRegistry` (core) and `KernelPortProbe` (`ear_runtime`) |
| `to_server.*` | `ToServer : HandoffConsumer` gains `attached(skipped)`, setting `consumed_` |
| `connection_watch.*`, `advertiser.*`, `pcm.hpp`, `peer_text.hpp`, `notes.hpp` | namespace only. `notes::notesDropped` goes (the framework says it) |
| `CMakeLists.txt` | the sources above; tests link the harness; `clock_test` links `ear_runtime` |

`file`: `File::start` asks for `cx.recording({path})` and `cx.handoff(record, ...)` and returns its `Recorder` code;
`File::build` makes `FileRun` with `ready.recording(path)` and `ready.handle()` and attaches its `WavWriter`, whose
`attached` calls `sf_open_fd` (today `file.cpp:76`, in `start`) and reports a failure with `handle.lost("cannot open
<path>: <libsndfile's reason>")`. Its tests keep their cases, now through the harness's recording fake and with the
WAV's header written on the handoff's (harness: the test's) thread.

sendspin's tests:

| Test file | Fate |
|---|---|
| `feed_test.cpp` (8 cases) | moves to `ear_runtime`'s tests, plus "a waiting writer returns at the close", "a write after the close waits its patience", `idle` before and after the close, and the sync thread's CPU time across a slow teardown (U7) |
| `port_claims_test.cpp` (3) | the claim case moves to core (`ClaimRegistry`); the two probe cases (a live listener, `TIME_WAIT`) to `ear_runtime`, plus a UDP port held with SO_REUSEADDR refused |
| `notes_test.cpp` (6) | `NoteGate`'s cases move to core's run handle tests; `printable` and the texts stay |
| `keys_test.cpp` (11) | open, narrow, create, symlink, size and replace-if-unchanged cases move to `ear_runtime` (`ConfinedStores`) as store cases over a declared rule; the record's own cases (`whyNotKeyRecord`, `load_blob`, `save_blob`, write failure as `key-write-failed`) stay, over a harness store; "making the node type narrows every key file" becomes `ConfinedStores::sweep` over sendspin's declaration |
| `settings_test.cpp` (6) | the type case runs through `TypeHarness<spec, Sendspin>`; parse cases unchanged |
| `code_test.cpp` (13) | over the spec code with `Io<spec>` built by the harness's buffers |
| `clock_test.cpp` (4) | builds the run's parts with `ear_runtime`'s real adapters beside its real engine (as it builds `FilterPortBlocks` today, `clock_test.cpp:270-282`) [V] |
| new `type_test.cpp` | through the harness: listening asks `claim::TcpListen{8928}`, dialling asks none; a held claim fails with today's text; a distrusted store fails with today's text and is not written; `build` attaches `to`; frames before `attach` and a capture stamp after it; the worker's first timer starts the client only when the clock advances; close runs the goodbye on the worker after the notifier let go, with no source left |
| `test/nodes/all_types.hpp` | `notYetInHarness` deleted; `allTypes()` calls `allNodeTypes()`; the config folder and `BootLog` go |
| `test/system/*` | unchanged lines; S3.7 checks every sendspin system test still passes on the new order of states (8) |

## 13. Interim seams, cleared

| Seam (section 2) | Cleared by |
|---|---|
| sendspin off `TypeSpec` / `TypeHarness` | section 12; `NodeTypeOf` then has no production user and stays for tests' hand-made types (`src/config/tests/test_types.cpp`, core's tests) |
| `NodeFolders`, `NodeFolders::log`, the folder argument | stores (6.8): `node_folders.hpp` deleted; the boot sweep is `StoreAdapter::sweep`, called by `main.cpp` |
| `StartContext::events()` beside `Ready::events()` | the run handle (5): only `ready.handle()` |
| A recipe holding threads built in `start` must ask for no outside node | P2 and the thread guard (7): no type code makes a thread; loops start at readiness and close at 4b whether or not `build` ran, so a run that never built holds nothing to join |
| A leaked run holds its port or file | claims (6.9) and the leaked-run record (6.10): abandoned ports are re-probed; stores and recording paths are refused until a late close, which starts the refused node |
| `notYetInHarness = {"sendspin"}` | section 10: deleted |
| `file` does I/O on the main loop (section 2) | recordings open on the I/O executor (6.11); the WAV header on the handoff's thread |

## 14. The boundary with slice 5

Slice 3 fixes the seams; slice 5 (control-plane.md) adds behaviour on them. Each fact has one home: the first column's
facts are this page's, the second column's are control-plane.md's, and the last column says where control-plane.md
uses this page's spelling (reconciled with its revision 2, section 20).

| Slice 3 delivers (this page) | Slice 5 builds on it (control-plane.md) | Where control-plane.md uses it |
|---|---|---|
| `RunHandle`: `node`, `generation`, `lost`, `log`, interim `say` with per-run pacing, reached by `ready.handle()` in `build` (G15, G17) | `Report<spec>` over the handle (status writes, events), made by `ready.report()`, which slice 5 adds to `ReadyOf`; removes `say` once sendspin's notes are split | its scope paragraph and 5.3: `ready.handle()`, `Report::handle()`, `build`, never a recipe |
| `StartOf<spec>`, `ReadyOf<spec>` and `build` (6.1) | the realtime ring, made by the framework per run, never acquired in `start`; the code writes it through `io.set`, `io.raise` and `io.log` (control-plane.md 5.4, 6.1) | 5.7: `gain`'s `start` takes `StartOf<spec>&` |
| Loops declared in the spec (`loop::dedicated<"sendspin">`, `.loops = Loops<...>{}`); `RunLoop::postFromOutside` and `closeToOthers` at step 1; queued own work destroyed unrun at 4b | action calls posted with `postFromOutside` to the loop the call names; a queued call's reply object answers `restarted` or `stopping` from its destructor at step 1; status fields bound to a loop by `writer::loop` | 5.2, 5.5, 7.6: `action::call<N, Args, Loop>` and `writer::loop(worker)` take a loop handle of the spec (G14), so a loop the spec lacks does not compile |
| "A thread a library owns" as the only non-loop thread (7); no run object owns a thread | status written by the run's code (`writer::rt`) or a loop of the spec (`writer::loop`); a library thread posts to the loop that owns the field | 5.2: no `writer::thread` |
| `NodeDiagnosticListener` codes and their texts: `ear.loop-slow`, `ear.loop-source-left` (6.4), `ear.claim-wait` (6.9), `ear.close-late`, `ear.close-overdue` with loops (6.10) | puts diagnostics on the wire (`node.diagnostic`) and in snapshots, each with its subject and fields | 4.6 and step 5.2 list all five |
| the feed's shortfall, the handoff's drops and its skipped frames counted | samples them as the status counters `ear.feedShortfall` and `ear.handoffDropped` (control-plane.md 4.6) | — |
| `notes-dropped <n>` as the framework's note | `ear.events-dropped` diagnostic | — |
| `log` records with `node` and `run` from a run's handle | `run` on other record kinds; `log.subscribe` on the wire comes later, not in slices 5 and 6 (control-plane.md 15) | — |
| `TypeSpec`'s whole member list and order (6.12): settings, banks, stores, loops, then slice 5's | adds `.status`, `.events`, `.actions`, `.logCodes` after `.loops` | 5.3 cites 6.12; one compile test covers every subset (U1) |
| the harness's fakes, by handle, and its clock rule (10) | captures status, events, actions and diagnostics; `h.invoke<action>(args)` runs on the harness's loops | 5.9: its `advance(ms)` is section 10's fake clock |

Not slice 3's: `restart` and every action, status tables, the wire schema, `typeSchemas`, parameters (slice 6),
recovery and readiness deadlines for outside nodes (slice 8). A store does not appear on the wire.

## 15. The cut: buildable steps

Each step keeps every test green and changes no audio path. S3.6 is the first step that changes what an operator sees
(section 8's changes 2 to 5); S3.7 adds change 1; S3.8 changes only the attribution of the library's lines.

| Step | What | Tests it adds |
|---|---|---|
| **S3.1 Run handle and facades** | `RunHandle` replaces `InsidesEvents`; `StartOf<spec>`, `ReadyOf<spec>`, `build`, `ready.handle()`; `white`, `card`, `rtp` and `file` take `StartOf<spec>&` (`file` gains `build`, still opening in `start` until S3.6); `NodeHost::logTo`; `say` paced in core (sendspin's `NoteGate` stays this step, so its notes are paced twice, harmlessly); `StartContext::events()` stays until S3.7 | a run's log carries node and run; a log after detach is dropped; a goodbye note is heard; pacing 32 then 1/s with `notes-dropped`, per run; `lost` never held back; a spec type returning a recipe throws; `build`'s signature held |
| **S3.2 Registry, kept values, mailbox, wake** | `ResourceAdapters`; `keep<T>`/`kept<T>`; `ear/core/mailbox.hpp` (sendspin includes it); `wake::named`; the `ear_runtime` package with `EventWakes` | a null adapter refuses its kind; kept values freed after the run object in reverse order and leaked with the run; a second `keep<T>` throws; wake gating (two rings, one write); the wake's drain-then-clear order (harness hook and real-thread stress); mailbox cases moved |
| **S3.3 Loops** | `loop.hpp`, `Loops`, `TypeSpec::loops`, `StrandPool`, `DedicatedLoops`; staging; `postFromOutside`, `closeToOthers`; teardown 4a/4b; the source count; harness's manual loops and clock rule | post order; `after`, `every` (coalescing), `onWake` (a pending wake fires once on arming), `onReadable`; staged sources run only after `build` returned and never after a failed one; queued work destroyed unrun at close; an in-flight handler finishes; outside work refused from step 1; a source left armed at 4b (assert in the harness, diagnostic in release); a dedicated loop that never drains is leaked at the deadline with "1 loops open"; `ear.loop-slow`; a stuck strand gets a replacement thread; the blocking assert on a strand; the harness's close failing at once with nothing to turn |
| **S3.4 Feed** | `cx.feed`, `ready.feed`, `RingFeeds` (a copy of sendspin's feed with `close` and `idle`; sendspin keeps its own until S3.7), closed at step 1, latency declared | feed cases moved; a waiting writer returns at the close; a write and an `idle` after the close wait their patience; `[0, capacity]` reaches the block's plan; a second declaration throws |
| **S3.5 Handoff consumer, I/O executor, stores** | `cx.handoff` with no consumer, `ready.attach`, the atomic consumer and `attached(skipped)`; `RunCode::toHandoff`; `IoThread` with shared-state jobs and its stop; `StoreRule`, `Stores`, `TypeSpec::stores`, `NodeType::stores()`; `ConfinedStores` with sweep, open at readiness, serialised `replace` and `replaceLater`, the 5 s deadlines | frames before attach discarded and counted, then `attached(n)`; a handoff never attached throws after `build`; attach twice throws; store open (missing, created, symlink, too big, distrusted with the file untouched, mode narrowed before judging); `replace` refused when changed; two writes in flight both commit in order; mode narrowed before a write; sweep over declared stores; open past its deadline and its late completion dropped; the I/O thread busy at stop; **one compile test of every `TypeSpec` subset spelling** (U1); compile-fail cases: two stores of one suffix, a bad suffix, `maxBytes` left out, a mode beyond 0777, `ready.store` and `ready.loop` of a handle the spec lacks, `cx.feed` on an input bank, `cx.handoff` and `ready.attach` on an output bank, `toHandoff` on an output bank |
| **S3.6 Claims and recordings** | `claim::TcpListen`, `claim::Udp`, `ClaimRegistry` judged at readiness step 1, `KernelPortProbe` (UDP without SO_REUSEADDR); implicit store, recording and `rtp` claims; recordings deferred onto the I/O executor; `file` opens its WAV in `attached`; the leaked-run record, late close, auto-start (G3); `ear.claim-wait`, `ear.close-late` in `output-records.json` | granted; refused (running holder, failed holder, across types) before any open or thread; waited (a removed node on 8928 and an added one on 8928 in one plan; `rec` removed and `rec2` added on one path) with `ear.claim-wait`; the 15 s deadline; abandoned port re-probed; a store and a recording path refused while a leaked run holds them, then freed by its late close and the node running by itself (a system test: a peer that never answers at stop, its goodbye finishing after the deadline, the same node then running); a run leaked in step 3 told to close; a run torn down while waiting leaves the queue; probe cases moved; system tests for visible changes 2 to 4 |
| **S3.7 sendspin moves** | section 12; `node_folders.hpp`, the folder argument and `notYetInHarness` go; the thread guard; `StartContext::events()` goes | `type_test.cpp`; sendspin in the all-type harness test; the guard on each spelling, a look-alike and the `/tests/` exclusion; every system test |
| **S3.8 The log patch** | `cmake/sendspin-log.patch`; `sendspin_patched_dep` with a patch list; the run object sets the sink and context | `ear_carried_patch_check` for both patches; a library line from the worker and one from `SsSync` arrive as `log` records with the node and run [U2]; two nodes' lines told apart; a line outside any scope is unattributed from `sendspin-cpp` |
| **S3.9 Words** | AGENTS.md (architecture table, threads table, adding a type, tests table, sendspin's library row, `rtp`'s held-port line, `file`'s recording line), the framework page's "Slice 3 as built", the `sendspin` skill | — |

## 16. Costs and risks

- **A wider core and a new package.** About 600 lines of interfaces, facades and registry in core, about 900 in
  `ear_runtime` (mostly moved from sendspin), against about 650 fewer in sendspin. One more library in the architecture
  table.
- **`ResourceRegistry`'s constructor changes**, so every `NodeHost` test's setup changes once (S3.2); every type's
  `start` signature changes once (S3.1).
- **Compile-time machinery grows**: stores, loops and wakes reuse the bank-name machinery; about twelve compile-fail
  cases (about 1.5 s each at one job).
- **One I/O thread.** A hung disk stalls every store and recording open and every store write; the 5 s deadlines make
  it visible, not harmless.
- **A shared strand pool** can let one node delay another for up to 1 s before a replacement thread starts; sendspin
  uses only dedicated loops, so nothing built today runs on a strand.
- **Thread names do not change**: loops named `sendspin` and `notify` give `ear-sendspin <node>` and `ear-notify
  <node>`, as today (`worker.cpp:190`, `310`).
- **A second carried patch** to rebase at each bump of the sendspin-cpp pin, touching about 15 places [U2].
- **Leaked runs keep stores and paths for longer** than today (refused until a late close), so a node behind a library
  that never finishes its stop stays failed until ear restarts; today it would start and share the file with the hung
  run. A late close restarts it by itself (G3).
- **Claims are by port, not by address.** Two `rtp` nodes on one port with different multicast groups are refused,
  though PipeWire binds each to its group (`module-rtp-source.c:265-309`) [V]. Multicast is untested (AGENTS.md); a
  later key `udp:<group>:<port>` can relax it.
- **`file` loses the first milliseconds** before its recording opened (visible change 4).

## 17. Rejected alternatives

| Alternative | Why not |
|---|---|
| Loops on `pw_thread_loop` in `ear_pipewire` (it already has timers, events and fd sources) | A loop has nothing to do with the engine; strands need a pool `pw_thread_loop` does not give; it would make every threaded type's tests start PipeWire's loop library |
| The run object keeps owning its threads (no loop primitive) | Against Q5; the recipe-never-ran rule stays; each type rewrites stop, wake and poll |
| Loops asked for in `start` by a runtime name (revision 1) | A name checked only at run time, a resource carried to the recipe by a hand-written capture, and slice 5's `call<..., "worker">` checked only at start (G14) |
| The recipe lambda kept for spec types (revision 1) | Every threaded type repacks resources by hand and a dangling capture compiles; `build` reaches them by handle (G15) |
| Mailbox (and any shared value) as a resource kind | No adapter, readiness, loss or teardown action: a variant member and an adapter for a constructor (G2) |
| Stores opened by the recipe or the run object | I/O on the main loop, or a run object that reads before it is ready (the framework page's round 2, #39) |
| Asynchronous store writes only | The library's `save_blob` returns a `bool` synchronously on its thread; a future waited on there is `replace` with extra steps (G7) |
| A recording claim judged inside `start`, refusal only (keeping the synchronous open) | No wait is possible, so I8's remove-and-add of one path cannot work, and `file` keeps I/O on the main loop (G16) |
| Claims judged after stores open (revision 1) | The open would read, narrow or create the very file a leaked run still holds, and start threads for a run then refused |
| Claims per type, as sendspin's | A port is ear-wide: an `airplay` node on 7000 and anything else on 7000 must see one another |
| The probe alone, no registry | Loses "held by node <n>", and refuses a remove-and-add of one port in one plan, which I8 allows |
| A closed feed that returns 0 at once to every caller | sendspin-cpp's sync thread paces itself on the sink blocking and would spin from step 1 to 4a |
| A handoff that does not drain until attached (frames kept, overflow counted as dropped) | The ring's oldest audio would reach the consumer first and `dropped=` would count frames nothing wanted; `attached(skipped)` keeps counts exact without either |
| A pass-through that also writes an unnamed handoff bank (revision 1) | Implicit: a pass-through asked for nothing of the kind; `RunCode::toHandoff` says it |
| The framework copies a feed straight into its bank | sendspin skips and counts frames the driver missed, and dates what it played; sugar for the simple case can come later |
| Passing the client at each `SS_LOG*` site | 533 sites: a patch no bump could carry (G8) |
| Leaving the library's lines unattributed | Against R9's intent and the framework page's section 15 item |
| The run handle in slice 5 | Slice 3 would ship loops with no generation-checked way out, and rename `events()` twice (G10) |
| A late consumer by type-level indirection (a `SourceSink` slot kept in `start`) | Every type whose consumer depends on its run object would write the same slot |
| A `setting::port` row kind for claims | A new row kind for one narrowing; claims taking the checked whole number do the same with no change to settings tables |

## 18. Gate questions

Final. Grouped; each has options and a recommendation with its reason. The page is written to the recommendations.

### A. Where things live

| # | Question | Options | Recommendation |
|---|---|---|---|
| G1 | Where do loops, wakes, feeds, the I/O executor, the probe and stores live? | (a) a new package `ear_runtime`; (b) `ear_pipewire`, on `pw_loop`; (c) split: loops in `ear_pipewire`, stores in `ear_folder` | (a): threads need no PipeWire, and host primitives get one home a type cannot link |

### B. What a type author writes

| # | Question | Options | Recommendation |
|---|---|---|---|
| G14 | How does a type get its loops? | (a) declared in the spec as handles (`loop::dedicated<"sendspin">`, `.loops`), started for every run, reached by `ready.loop(h)`; (b) asked for in `start` by a runtime name (revision 1) | (a): names checked at compile time like banks and stores, nothing carried by hand, and slice 5's `call` on a missing loop fails to compile rather than at start |
| G15 | How is the run object made? | (a) the behaviour's `static build(ReadyOf<spec>&, const CheckedOf<spec>&)`, with spec-typed `StartOf`/`ReadyOf` facades; (b) the recipe lambda returned by `start` (slice 2's) | (a): resources by handle, wrong banks or stores refused at compile time, no dangling capture; slice 2's lambda stays for hand-made `NodeTypeOf` types only |
| G2 | Is a mailbox a resource kind, or a value the run keeps? | (a) `cx.keep<Mailbox<T>>()`; (b) a `mailbox<T>` kind as the framework page's 7.3 says | (a): it has no adapter, readiness, loss or teardown action |
| G4 | A handoff whose consumer is part of the run object | (a) `ready.attach` in `build`, frames before it discarded and counted to the consumer as `attached(skipped)`; (b) the handoff holds frames until attached, overflow dropped; (c) the type builds the consumer in `start` | (a): sendspin's capture stamps stay exact, `dropped=` counts only what was wanted and lost |
| G17 | Naming the handle on `ReadyOf` | (a) `ready.handle()`; (b) `ready.run()` (revision 1) | (a): `run()` reads as a verb; control-plane.md is written to it, `Report::handle()` included |

### C. Lifetimes, claims and files

| # | Question | Options | Recommendation |
|---|---|---|---|
| G3 | A store or recording path (or a port whose probe fails) held by a leaked run | (a) refused, with a detail that says it starts when that run closes, and the late close starts it; (b) waits in `starting` with `ear.claim-wait`, up to a long deadline (60 s), then failed with no restart; (c) refused until a late close or a restart, nothing restarts it (revision 1); (d) given (replace-if-unchanged guards the text; two runs share the identity) | (a): the operator sees a failed node with the real reason at once, I3's "never-ready is a fact" holds, and it recovers by itself the moment it safely can; (d) gives Music Assistant two clients with one identity and corrupts recordings |
| G16 | A recording's claim and `file`'s I/O | (a) the recording is a deferred kind: claim judged first, opened on the I/O executor, the WAV header written on the handoff's thread (P2 for every type; `file` loses the first milliseconds, failures of libsndfile come after `running`); (b) the open stays in `start`, its claim judged there, refusal only (no remove-and-add of one path in one plan; `file` keeps main-loop I/O as a named seam) | (a): one rule for every type, and I8 holds for paths as for ports |
| G13 | Readiness deadlines added here | (a) stores and recordings 5 s, claim waits 15 s; (b) none until slice 8 | (a): every wait added here is new in this slice, and I3 forbids an endless one |
| G7 | Store writes | (a) `replace` (blocking, from dedicated loops and library threads, 5 s bound) and `replaceLater` (any loop), both serialised on the I/O executor; (b) asynchronous only | (a): the library's `save_blob` is synchronous on its own thread |

### D. Logs and notes

| # | Question | Options | Recommendation |
|---|---|---|---|
| G10 | The run handle | (a) slice 3, with `log`; (b) slice 5 | (a): loops need a generation-checked way out now |
| G5 | Note pacing in slice 3 | (a) the framework paces every type's `say`, per run, with the text `notes-dropped <n>`; (b) leave it in sendspin until slice 5 | (a): sendspin deletes `NoteGate` now; slice 5 turns it into `ear.events-dropped` |
| G8 | Attributing the library's lines | (a) a per-thread context inherited by the library's threads and its socket callbacks; (b) the client at each call site; (c) leave them unattributed | (a): about 15 places to carry instead of 533 |
| G11 | Boot sweep findings | (a) `log` records from the type's name, as today; (b) a diagnostic now | (a): diagnostics reach the wire only in slice 5 |

### E. Threads and guards

| # | Question | Options | Recommendation |
|---|---|---|---|
| G9 | Strand pool defaults | (a) `min(4, cores)` threads, slow at 100 ms, a replacement at 1 s, at most 16; (b) one thread per strand (no pool) until a type uses strands | (a), measured on the Talos CPU when known |
| G12 | A guard against threads in `src/nodes/` | (a) refuse `std::thread`, `std::jthread`, `std::async`, `pthread_create`, `fork` outside `/tests/`; (b) review only | (a): P2 is then checked on every build |
| G6 | `notYetInHarness` | (a) delete it; (b) keep the mechanism, empty | (a): a type the harness cannot run is a framework gap, to be closed, not listed |

## 19. Open items

- **U1.** Aggregate deduction for `TypeSpec{.settings = ..., .banks = ..., [.stores = ...], [.loops = ...]}` with
  every later template parameter defaulted: GCC 14 should deduce a defaulted parameter for an omitted designated member.
  S3.5 adds one compile test of every subset spelling (none, `.stores` only, `.loops` only, both) before any type uses
  them; slice 5 extends the same test with its members (control-plane.md's own [U] in its 5.3 is this one). The fallback
  is a consteval `makeSpec(...)` with the same members in the same order.
- **U2.** That every `SS_LOG*` line sendspin's tests provoke runs inside a scope: S3.8's system test counts
  unattributed `sendspin-cpp` records over the pairing and connection tests, and each one found is a missed entry point.
  The patch's size (about 15 places) is an estimate from the entry points listed in section 9.
- **U3.** That the library calls the persistence provider only on threads in the client's scope, the worker among them
  (`client.cpp:1648-1741`, `player_role.cpp:751-778`, `record_store.cpp:43` [V] are the call sites; their threads are
  not yet traced). S3.7 asserts in `save_blob` that it runs on a dedicated loop or a library thread, never a strand.
- **U4.** sendspin's system tests and a `running` line before a corrupt key file's failure (8): S3.7 runs them.
- **U5.** The strand pool's sizes on the Talos nodes' CPU, unknown until the hardware is (AGENTS.md).
- **U6.** Whether `onReadable` is wanted by sendspin's mDNS socket instead of answering in the 10 ms tick: not needed to
  move; the tick keeps today's behaviour, and the change can follow.
- **U7.** The feed's close contract (6.5) keeps sendspin-cpp's sync thread asleep between calls from step 1 to 4a. S3.4
  proves it on `RingFeeds` (a caller looping on a closed feed uses under 1% of a core over 2 s); S3.7 measures the real
  sync thread's CPU time across a teardown held 2 s in step 3 by a slow consumer.
- **U8.** Upstream interest in the log hook; until then it is carried.
- **U9.** Closed: the boundary with control-plane.md was reconciled on 2026-11-08 (section 20). Both pages are written
  to G14, G15 and G17's recommendations; an owner's answer against one changes both.
- **U10.** That `keep<T>`'s static checks catch the common mistakes; a `T` that owns a thread inside another type is not
  caught, and the guard of section 7 covers `src/nodes/` instead.

## 20. Review record

Revision 1 was reviewed by two reviewers: realtime and concurrency (RC1 to RC18) and DX and API (DX1 to DX17). Each was
checked against the code at `f677a50` before acting on it.

| Finding | Outcome | Where, and why |
|---|---|---|
| RC1 late handoff consumer misdates captured audio | fixed | Verified (`sendspin_code.cpp:25-26`, `to_server.cpp:20-36`, `handoffs.cpp:24-25`). `attached(skippedFrames)` and the atomic release/acquire consumer (6.6); a harness test of a stamp after attach (12); G4 |
| RC2 a source can fire after `closed`; sendspin's `after(0)` never cancelled; destruction rule contradicts step 5 | fixed | The source contract, counting and `ear.loop-source-left`, destruction on any thread once the loop is closed, `[[nodiscard]]` (6.4); sendspin's `close` resets `starting_` (4.1) |
| RC3 recording claims cannot be judged at readiness; `file` does I/O in `start` | fixed | Verified (`node_host.cpp:137-141`, `main.cpp:84`, `file.cpp:76`). Recordings are a deferred kind; `file` opens its WAV in `attached` (6.11, 12); G16 |
| RC4 a closed feed and the lost stop-fd wait make the sync thread spin | fixed | Verified (`sync_task.cpp:56-58`, `worker.cpp:125-131`). The close contract (a wait in progress ends; a later one sleeps its patience) and `FeedWriter::idle` (6.5); U7 a test. The suggested "idle returns early once closed" was not taken: it would spin after the close |
| RC5 the two pages disagree on `TypeSpec`'s member order | fixed | One home, 6.12; control-plane.md cites it (14, U9); one compile test of every subset (U1) |
| RC6 actions can run between 4a and 4b | fixed | `postFromOutside` and `closeToOthers` at step 1 (6.4, 6.10, 14) |
| RC7 the late close has no mechanism and restarts nothing | fixed | Verified (`node_host.cpp:191-221`, `447-464`). The leaked-run record, a step-3 leak told to close, `ear.close-late`, the refused node started by the late close (6.9, 6.10); the wait-or-refuse choice is G3 |
| RC8 claims judged after stores open | fixed | Readiness order: claims, stores and recordings, loops, `build`; outside nodes in parallel after claims (6.1) |
| RC9 the wake's drain/clear order and a ring before arming | fixed | 6.3: `exchange` then write; `read`, clear, handler; a pending wake fires once on arming; harness and stress tests |
| RC10 a store open past its deadline and a hung I/O thread at stop | fixed | Jobs hold shared state, generation-checked completions; a bounded stop and `_Exit` (6.7) |
| RC11 a UDP probe with SO_REUSEADDR misses rtp-source | fixed | Verified (`module-rtp-source.c:252` in PipeWire 1.4.2). UDP probed without SO_REUSEADDR or SO_REUSEPORT (6.9) |
| RC12 `rtp` claims nothing; a failed holder is not covered | fixed | `RtpReceiverRequest` claims `udp:<port>`; a failed-holder row (6.9); visible change 3 (8); the per-port limit in 16 |
| RC13 a claim wait gives no reason | fixed | `ear.claim-wait` with its text, in `output-records.json` from S3.6 (6.9, 15) |
| RC14 a forgotten `attach` discards silently; implicit pass-through writes | fixed | A never-attached handoff throws after `build`; `RunCode::toHandoff` (6.6) |
| RC15 the rtpsend example races on its socket | fixed | One thread owns the socket: the handoff's (`attached`, `consume`, `finish`) (4.2) |
| RC16 the harness's close waits 10 s of real time | fixed | Fails at once when every loop is idle, naming loops and armed sources (10) |
| RC17 store writes are not serialised | fixed | Every write is an I/O job in order; `text()` changes at commit; `replace` bounded at 5 s (6.8) |
| RC18 small inconsistencies (log after detach, strand names, `BankRef`, `maxBytes` default, `std::async`, keys parsed before arming, line counts) | fixed | 5 (log wording); 6.4 (only dedicated threads named); 6.1 (handles everywhere); 6.8 (`maxBytes` required at compile time); 7 (`std::async`); 4.1 (`parseTrusted` before construction); 2, 11, 16 (about 560 of copies, about 650 in all) |
| DX1 recipe arming races; failed `readKeys` destroys a running source | fixed | Staging (6.4); keys parsed before the run object exists (4.1) |
| DX2 recording claim after the file was truncated | fixed | As RC3 (6.11); G16 |
| DX3 stores open before claims | fixed | As RC8 (6.1) |
| DX4 teardown of a run not yet ready, and a failed start | fixed | Step 1's not-ready clause and the "torn down before it was ready" paragraph (6.10); "A start that fails" and "A `build` that fails" (6.1) |
| DX5 `Ready` and requests not tied to the spec | fixed | `StartOf<spec>` and `ReadyOf<spec>` with static checks; compile-fail cases only for what they refuse (6.1, 15); G15 |
| DX6 hand plumbing; loop names are runtime strings | fixed | Loops in the spec as handles, `build` reaching everything by handle, kept values by type, wakes by handle (6.1 to 6.4); slice 5's `call` takes the handle (14); G14 |
| DX7 the stop-eventfd wait has no replacement | fixed | `FeedWriter::idle` and the close contract (6.5); sendspin's port (4.1). "Returns early once closed" was not taken, as for RC4 |
| DX8 implicit pass-through to a handoff; adapter change unspecified | fixed | `RunCode::toHandoff`; `passThrough` keeps one pair or more; the atomic consumer, `finish` only if attached (6.6) |
| DX9 visible changes undercounted; claim key normalisation | fixed | Five visible changes with their lines (8); a recording's key normalised (6.9) |
| DX10 harness keyed by strings; behaviours undefined | fixed | Handle-typed calls, the clock rule, readiness defaults, `waitClaim`/`releaseClaim`, `readable(fd)`, `replaceLater` timing (10) |
| DX11 the thread guard would match sendspin's tests | fixed | Verified (`code_test.cpp:272`, `feed_test.cpp:80`). The guard skips `/tests/`, and that exclusion is tested (7) |
| DX12 "a thread the run object owns" contradicts P2 | fixed | "A thread a library owns" throughout; control-plane.md has no `writer::thread`: a library thread posts to the loop that owns the field (14) |
| DX13 renames and the dropped mDNS claim missing from section 3 | fixed | Rows for the claim, mailbox, loop and feed spellings, and the mDNS sentence (3, 6.9) |
| DX14 `keep<T>` takes any `T`; order unstated | fixed | Reverse order of `keep`; static checks against run objects, consumers and threads; the destructor rule (6.2); U10 |
| DX15 `maxBytes` silently 0 | fixed | Refused at compile time, as is a mode beyond 0777 (6.8, 15) |
| DX16 "No I/O" in `start` is false for recordings | fixed | True now: recordings deferred (6.11, 7) |
| DX17 `ready.run()` reads as a verb; ports narrowed by hand; pacing scope | fixed | `ready.handle()` (G17); claims take the checked whole number (6.9; a `setting::port` row kind rejected, 17); `say` paced per run (5) |

**Boundary reconciled with control-plane.md revision 2** (2026-11-08), each fact given one home and cited from the
other page. Here: section 14 cites control-plane.md for the realtime calls (`io.set`, `io.raise`, `io.log`, not
`rtlog`, `rtevent`) and the ring the framework makes per run (not acquired at start), for `Report<spec>` made by
`ready.report()`, for no `writer::thread` (a library thread posts to the loop that owns the field), for the status
counters, and for `log.subscribe` coming after slices 5 and 6; section 5's split of the handle and the ring says the
same. The claim wait keeps this page's spelling, `ear.claim-wait`, in both. U9 is closed, G17 no longer calls
control-plane.md's spelling `run()`, and 6.4 and 6.12 cite control-plane.md's sections as they now stand.
