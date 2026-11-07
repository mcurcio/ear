# The node framework: the primitives every node type builds on

Status: proposal, revision 2, 2026-11-07. Revision 1 went to three independent reviews (section 16); this revision
answers them. Nothing here is built.
Layer: system design (domain model and public interfaces), beneath the agreed system design
(`docs/design/pipewire-graph.md`) and over the module designs it would change (`docs/design/batch2.md` sections 4 and 6,
`docs/design/sendspin.md` sections 3 to 8).

[V] = verified in the source named, as of `3ab159a`; [U] = unverified, so a slice's test checks it or section 15 keeps it
open.

## 1. The owner's words

| # | His words | Date, where | What it means here |
|---|---|---|---|
| R1 | "what are the core primitives that ear should host so that new nodes can be added simply and only require defining the truly custom bits that its node type demands" | 2026-11-07, chat | A node type is its custom part and nothing else; everything shared is the framework's |
| R2 | "consider how to abstract configuration and control plane events so that nodes can build on a shared base primitive layer" | 2026-11-07, chat | Configuration and control-plane events are framework subsystems, not per-type code |
| R3 | "Is it a virtual node base class? Are there several distinct subsystems, like one for config and one for events? How much do nodes [have] to write to handle bus connections?" | 2026-11-07, chat | The questions this page answers (sections 3, 7, 8 and 9) |
| R4 | "can i add my own sources and sinks?"; "the core will be \"built into ear\"" | 2026-10-29, `pipewire-graph.md` R11 | Types stay built in; adding one must be cheap |
| R5 | "i will want multiple shairport instances" | 2026-10-29, `pipewire-graph.md` R9 | Many nodes of one type, and a third kind of outside thing (a program) |
| R6 | "in a general purpose sources-and-sinks graph, its always just 'running'" | 2026-10-29, `pipewire-graph.md` R2 | Nothing here may make a port block stop processing |
| R7 | Outputs on different devices stay aligned, for "seperate zones" | 2026-10-29, `pipewire-graph.md` R3 | Latency and clock facts belong to the audio contract |

Design rules 1 to 3 of `pipewire-graph.md` hold unchanged: one owner per piece of state, and each health judgement its
owner's; a node is self-contained, reaching the engine only through powers the framework grants; the graph always runs.

## 2. The problem, measured

What each type writes today [V]:

| Type | Lines | Classes it writes | What is really its own |
|---|---|---|---|
| `white` | 73 | `AudioCode`, `NodeInsides`, `NodeConfig`, `NodeTypeOf` | a noise loop |
| `card` | 105 | the same four | its settings, one rule across them, "open this card", and a pass-through whose indices it works out by hand (`card.cpp:38-43`) |
| `rtp` | 114 | the same four | its settings, "receive this stream", the same hand-indexed pass-through |
| `file` | 190 | the four, a descriptor wrapper, a `HandoffConsumer` | its settings, the path rule, libsndfile |
| `sendspin` | about 1,900 | the four, and the primitives below | the protocol, pairing, the time filter, mDNS |

`sendspin` had to build, privately, about 560 lines of primitives that any threaded or networked type needs [V]:
`Mailbox` (a seqlock latest-value slot), `Feed` (the handoff's reverse), `EventFd`, `NoteGate` (rate-limited notes),
`PortClaims` (which node holds which TCP port, held by the *type* across its nodes), `KeyStore` and `narrowKeyFiles` (a
0600 file per node in the config folder, swept at boot), `ConnectionWatch` (timers), and three threads of its own: the
worker (a 10 ms `poll()`), the notifier, and the library's sync thread, which writes the feed (`worker.cpp:123-135`).

The framework's own seams are closed in three places [V]:

- `NodeGrant` has one method per kind of outside node (`soundCard`, `rtpReceiver`), and `NodeHost`'s constructor takes
  one factory per kind (`node_host.hpp`). A program (R5), a network sink or an RTP sender each edits `ear_core`,
  `NodeHost`, `ear_pipewire` and `main.cpp`.
- The control plane out of a node is `InsidesEvents::say(std::string)` and `lost(std::string)`. About 11 of sendspin's 17
  note texts are state written as pairs of sentences (`connected`/`disconnected`, `pairing-code`/`pairing-code-cleared`,
  `stream`/`stream-ended`, `sending`/`sending-stopped`, `id`, `dialling`, `listening`). Notes reach standard output only,
  so the editor cannot show a pairing code.
- There is no control plane *into* a node except its settings. A failed node starts again only when a setting really
  changes (an update with the same settings changes nothing, `desired_graph.cpp`), so restarting one means changing a
  setting and changing it back, or restarting ear. `NodeState` has no `recovering` (`node_lifecycle.hpp:8`).

## 3. The answer, in one paragraph

A node type is **declarations, one behaviour, and acquisitions**. The declarations are constexpr tables in one
`TypeSpec`: settings (as today, plus a rule across them), bank names, status fields, events, actions, stores, recovery
and latency. The behaviour is one plain class: `ports` (member counts from the settings), `start` (acquires resources and
returns the run's code), `process` if it makes or changes audio, and the run object it builds for work off the realtime
thread. The acquisitions are framework-owned resources, each an RAII handle the framework readies, watches and closes in
dependency order, asynchronously. There is no base class for the author beyond the existing type-erasing `NodeTypeOf`,
and no mixins: capabilities combine freely, so they are acquired, not inherited. Five subsystems serve every type:
**configuration and parameters**, **resources**, the **control plane**, **audio and clock**, and **recovery**. For bus
connections a type writes nothing.

## 4. Ubiquitous language

| Term | Meaning | Today's name, if different |
|---|---|---|
| **Node type** | A built-in kind of node: its type spec and its behaviour, found by its folder in `src/nodes/` | `NodeType` + `NodeTypeOf` |
| **Type spec** | A type's declarations, pure and constexpr (`TypeSpec`; `NodeSpec` stays the graph's name+type+settings, `graph_spec.hpp:25`) | the settings table, plus `NodeConfig::ports()` |
| **Node** | One named instance of a type in the desired graph; the aggregate root of everything below | — |
| **Settings** | A node's *desired* state, kept in `ear.conf`, judged by the desired graph | `NodeSettings` |
| **Parameter** | A setting declared `live`: it also has a realtime value that can move without a restart | — |
| **Port block** | The node's one `pw_filter`, made once for the node's life; owner of its public ports and the routes' ends | as today |
| **Bank** | A named, directed group of ports, public or inside; its name and direction in the spec, its members from the settings | `PortBank` (public only); inside ports are a flat list today |
| **Run** | One start of a node, from `start` to the end of its close; restarted behind the same block | `NodeInsides` |
| **Run object** | The type's own object for a run (sendspin's client, pairing, time filter), with the threads it owns | members of `NodeInsides` |
| **Context** | What `start` sees: the format, the node's name, and the acquire calls | `NodeGrant` |
| **Resource** | Something a run acquired, owned by the framework, closed with the run | outside node, handoff |
| **Resource kind** | A class of resource with its own adapter (section 7.3) | one method each on `NodeGrant` |
| **Code** | What runs on the realtime thread for one run: `prepare`, `process`, `release` | `AudioCode` |
| **Loop** | A run's serial executor off the realtime thread: a strand, or a dedicated thread | sendspin's worker |
| **Status** | A node's *observed* state, field by field, each field with one declared writer; never health | `say` texts that are state |
| **Event** | A node's *edges*, by declared code | `say` texts that are edges |
| **Action** | A command into a node; changes no desired state | — |
| **Health** | `NodeState` and its reason: the framework's judgement alone | as today |
| **Driver** | The resource clocking the graph: the dummy driver, or a linked sound card | as today |

## 5. Bounded contexts and the context map

```mermaid
flowchart LR
  GD["Graph definition<br/>ear_config + DesiredGraph<br/>settings: check, plan, commit"]
  subgraph Core["Core domain: node framework (ear_core)"]
    NF["Node aggregate<br/>runs, lifecycle, health, recovery"]
    REG["Resource registry<br/>kinds by value"]
    CLM["Claims and stores<br/>(domain service, ear-wide)"]
    CP["Control plane<br/>one ordered channel per node"]
    AU["Audio and clock contract<br/>banks, Io, cycle, latency"]
  end
  T["Node types (src/nodes/*)<br/>spec + behaviour"]
  PW["Audio engine (ear_pipewire)"]
  FS["Confined folders (ear_folder)"]
  PS["Program supervisor (future)"]
  WIRE["Control surface<br/>ear_control, ear_server, web/<br/>wire schema"]
  GD -- "checked settings, parameter values" --> NF
  T -- "SPI" --> NF
  NF --> REG --> PW
  REG --> FS
  REG --> PS
  NF --> CLM
  NF --> CP --> WIRE
  NF --> AU --> PW
```

- **Graph definition** stays the one judge of settings. It asks each type's spec to check them (the table walk, then the
  type's rule across settings, as `make` does today), and it carries parameter changes (section 8.5) in arrival order with
  every other request. It never sees resources, status or actions.
- **Node framework** owns the node aggregate and the subsystems. Types reach it only through the SPI (section 7) and
  cannot extend its model.
- **Claims and stores** is a domain service, not part of one node's aggregate: a TCP or UDP port, an mDNS name and a
  stored file are ear-wide, so their consistency rule spans nodes (section 6, I8).
- **Audio engine, confined folders and the program supervisor** sit behind one adapter per resource kind, which keeps
  PipeWire, descriptors and process ids out of `ear_core` (as today: no PipeWire type in a public header).
- **Control surface** renders what the framework publishes (section 8.7) and judges nothing.

## 6. The node aggregate and its invariants

The node is the consistency boundary for everything one run acquires; only the framework changes it, on the main loop.
Resources are a closed `std::variant` of kinds (section 7.3), not a class hierarchy.

```mermaid
classDiagram
  class Node {
    name: NodeName
    type: NodeType
    health: NodeState + reason
    recovery: attempts, next try
    block: PortBlock
    status: slots, one writer each
  }
  class Run {
    generation: n
    code: Code
    resources: list of variant~Kind~
    runObject: the type's, with its threads
  }
  Node "1" --> "0..1" Run : current
```

Invariants (each the framework's to keep, and a test's to prove):

- **I1. One block, for the node's life.** Settings changes restart the run behind it; a port change replaces the node (as
  today). A lost block is final (`node_lifecycle.hpp:11`): recovery never remakes it.
- **I2. Health is the framework's.** A type reports facts (a resource lost, `fail(detail)`); it never sets `NodeState`.
  Status never counts as health. Every fact crosses to the main loop before it acts, so no thread ever tears down the run
  it runs in (as `Channel::lost` does today, `node_host.cpp:153-157`).
- **I3. Ready is the conjunction, and never-ready is a fact.** The node is `running` when the block and every resource of
  the run are ready. Each resource kind that becomes ready later has a readiness deadline, and missing it is a loss
  (section 7.3); "never ready, never lost" cannot last.
- **I4. Nothing the realtime thread can reach is freed while it can reach it.** Every teardown starts with a barrier on
  the block's data loop: `drive(nullptr)` for a restart, the block's destruction for a removal or stop (both are blocking
  invokes, `port_blocks.cpp:97-127` [V]).
- **I5. A resource outlives every thread that can touch it.** Teardown is by dependency, not by reverse acquisition, and
  every step that waits on another thread is asynchronous. The main loop never joins a thread:
  1. the barrier (I4), then `release`;
  2. outside nodes torn down (they touch only the engine);
  3. handoffs closed: each drains and calls `finish` on its own thread, then reports (as today, `handoffs.cpp:44-91`); a
     consumer may still use the run object, which is alive;
  4. the run closes: its loops take no new work, in-flight handlers and actions finish (an action gets its reply), the
     run object stops its threads (the library's included) and says goodbye on its own thread, then calls `closed` from
     any thread (as `NodeInsides::close` today, `node_type.hpp:126-128`);
  5. on the main loop: the run object is destroyed, then feeds, mailboxes, parameter slots and wakes are freed, then stores
     close, then claims are released, last.
- **I6. Nothing from an old run lands on a new one.** Status, events and facts are written through a per-run handle that
  carries the run's generation; the main loop drops anything from an older generation, and drops everything once step 4
  began. Status slots are reset only after step 4 completes. The handle is shared-owned so a late writer never touches
  freed memory (as `Channel` is today, `node_host.cpp:142-190`).
- **I7. One run at a time.** A restart or recovery completes I5 before `start` is called again.
- **I8. A claim has one holder.** A port, mDNS name or store held by an older run, or by a removed or renamed node still
  closing, is not given to a newer one: the newer node's start waits until the older run's step 5 released it (today's
  `olderGoing` rule, `node_host.cpp:59`, generalised from names to claims). A claim held by a running node refuses
  (`startFailed: cannot listen on port <p>: held by <node>`). So removing a node on port 8928 and adding another on 8928
  in one plan works: the new one starts once the old one let go.
- **I9. Names are the node's.** Every engine object a resource makes is named `<node>.<suffix>`; every stored file is
  `<type>-<node>.<suffix>`; the registry refuses two resources of one run with one suffix.

## 7. The SPI: what a type writes

### 7.1 The type spec (declarations)

```cpp
// rtp, in this proposal
constexpr auto channelsRow = setting::required("channels", setting::number(1, 64));
// ... the other rows, as today
constexpr SettingsTable<addressRow, portRow, formatRow, channelsRow, rateRow, latencyRow> settings{"rtp"};
constexpr auto stream = bank::output("stream");
constexpr auto fromRtp = bank::insideInput("rtp");
constexpr auto receiving = status::flag("receiving", writer::resource);   // written by the receiver's adapter

constexpr TypeSpec spec{
	.settings = settings,
	.banks = Banks<stream, fromRtp>{},
	.status = StatusTable<receiving>{},
	// .events, .actions, .stores, .recovery, .latency: optional; defaults are empty, recovery::none, no latency
};
```

- **Bank names and directions are constants in the spec**; `ports(checked)` gives only each bank's members. So a
  misspelt bank in `process` does not compile, and the members' count is checked once at `prepare`.
- **A rule across settings stays at parse**: an optional `static std::expected<void, std::string> check(const Checked&)`,
  run by `parse` after the table's walk, so `card`'s "playback or capture above 0" is still `invalid-settings` and never
  reaches `ear.conf` (today `card.cpp:94`).
- **Rows are read by handle**: `s.get<channelsRow>()`, as today (`settings_table.hpp:219`). A row's value type does not
  identify the row, so there is no `s[row]` sugar.
- **Two new setting kinds are prerequisites** for types the framework claims to serve: `decimal` (gain in dB, ratios;
  it touches knobly's reader, the wire and the editor) and **indexed rows**, a family `gain.<i>` sized by a count row
  (a mixer with one gain per input). Both reuse the row machinery: one checker for settings, action arguments and
  parameters.
- **Stores are declared by suffix** (`stores = Stores<store::file("key", mode0600)>{}`): the framework names the file,
  opens it off the main loop, owns the boot sweep that narrows every such file in the folder (today `narrowKeyFiles`,
  `sendspin_type.cpp:124`) and hands the run a handle.
- The spec is the one source of: the graph file's setting kinds, the wire schema (8.7), the type's status-line
  vocabulary, and the compile-time handles the behaviour uses. Misspelt handles do not compile, as a misspelt setting row
  does today (`src/core/tests/compile_fail/`).

### 7.2 The behaviour (one plain class)

```cpp
struct Rtp {
	static Members ports(const Checked& s);                     // pure: {stream: n, fromRtp: n}
	static Started start(Context& cx, const Checked& s);        // main loop; never blocks, does no I/O
};
NODE_TYPE(rtp, spec, Rtp);
```

`NODE_TYPE` expands to the named function the generated type list calls (today's `make<Type>NodeType`), never to static
self-registration, which a static library drops at link time; it keeps the compile-time check that the table is named
for its folder.

`Started` is a failure (`startFailed: <detail>`) or the run: its code, and optionally its run object.

| The run's code | When |
|---|---|
| `passThrough(fromBank, toBank)` (one or more pairs) | audio joined to an outside thing: `card`, `rtp` |
| a class with `prepare`, `process`, `release` (a concept, erased once) | a type that makes or changes audio |

There is no `NodeConfig` or `NodeInsides` to write. The **run object** is any class with `close(done)`, built in `start`
from acquired resources; the framework owns it and closes it at I5 step 4. It holds the threads the type's library owns,
and it alone joins them, off the main loop.

### 7.3 The resources (acquisitions)

`Context::acquire(request)` on the main loop, inside `start` only (else `std::logic_error`, as `NodeGrant` today). The
list is a closed `std::variant` in core on purpose: a kind changes what the framework promises. Each kind has one adapter.

| Kind | Request | The type gets | Ready when (deadline) | Lost when | Replaces |
|---|---|---|---|---|---|
| Sound card | `outside::soundCard{card, direction}`, an inside bank | nothing to hold | its PCM runs and every link is active (deadline) | ERROR, proxy removed, a link failed, or the watchdog (9.5) | `NodeGrant::soundCard` |
| RTP receiver | `outside::rtpReceiver{...}`, an inside bank | nothing; it writes its `receiving` field | its node runs and every link is active (deadline) | as above | `NodeGrant::rtpReceiver` |
| Program (future, section 7.5) | `outside::program{...}` | nothing | its process runs | its process exits | — |
| Handoff | `handoff{bank, frames, consumer}` | an `RtWriter&` for `process` | at once | — | `NodeGrant::handoff` |
| Feed | `feed{bank, frames}` | an RT reader for `process`, a writer that may wait for room | at once | — | sendspin's `Feed` |
| Mailbox | `mailbox<T>{}` | an RT writer, a reader off RT; newest wins | at once | — | sendspin's `Mailbox` |
| Wake | `wake{}` | an RT ring, at most once until answered; a handle a loop waits on | at once | — | sendspin's `EventFd` |
| Loop | `loop::strand`, `loop::dedicated` (any number) | a serial executor: post, timers, fd watches, wakes | its thread runs | — | sendspin's worker and notifier, `ConnectionWatch` |
| Store | declared in the spec (7.1) | read and replace-if-unchanged, off the main loop | once opened and trusted (asynchronous) | a file it cannot trust | `KeyStore`, `narrowKeyFiles` |
| Recordings file | `recording{path}` | a descriptor beneath the recordings folder | at once | — | `NodeFolders.recordings` |
| Port claim | `claim::tcpListen{port}`, `claim::udp{port}` | nothing | at once, or waits (I8), then probed | — | sendspin's type-level `PortClaims` |
| mDNS advertisement (when a second type needs it) | `advertise{service, port, name, txt}` | nothing | first announcement sent | — | sendspin's `Advertiser` |

- **Readiness deadlines.** Today a port held by another program leaves an `rtp` node `starting` for good, because
  PipeWire only logs the failed bind (AGENTS.md); a card whose PCM never runs gives no event either. Each kind with a
  later readiness has a deadline (a default per kind, at least the kind's own start time) after which the framework
  counts it lost, with the kind's reason.
- **Adapter-sourced status.** A resource kind may write fields of its node's status table that the spec gives
  `writer::resource`. The RTP adapter binds the rtp-source node's proxy and copies `rtp.receiving` (a bound proxy sees
  it; the registry does not [V: `rtp.md` finding h]), and passes a short `cleanup.sec` so silence is seen within seconds,
  not 60 [U: the value].
- **Blocking feed writes** are legal only from a dedicated loop or a thread the run object owns, never from a strand
  (asserted in debug builds).
- **Folders become resources**; `NodeFolders` and `make<Type>NodeType(const NodeFolders&)` go.
- **Live properties of an outside node** (an ALSA adapter's volume `Props`) belong to its kind's adapter, set through a
  parameter (8.5); not designed here.

### 7.4 Threads, as one contract for every type

| What the type writes | Runs on | May |
|---|---|---|
| Rows' parse functions, `check`, `ports` | any thread, pure | nothing else |
| `start` | the main loop | acquire; construct. No blocking, no I/O (stores open on the framework's side) |
| `prepare`, `release` | the main loop, never with `process` | allocate |
| `process` | the realtime thread | the RT ends of its resources and `io` only. No locks, allocation, retry loops on a shared value, or syscalls other than a wake |
| Loop handlers, action handlers | the run's loop, serially | block only on a dedicated loop |
| The run object's own threads | its threads | anything; the object joins them in `close` |
| `HandoffConsumer::consume`, `finish` | the handoff's thread | block on I/O |

After `start` returns, no type code runs on the main loop (sendspin's discipline today, `docs/design/sendspin.md`
section 6). Debug builds assert the thread at every `acquire`, status write, event, loop post and feed write, as CLAP's
thread-check does.

**Strands by default, dedicated on request.** A strand runs on a small framework pool, so many nodes (R5) do not cost a
thread each; a handler that runs longer than a bound is reported. A type whose library blocks or must be polled asks for
`dedicated` loops, as many as it needs: sendspin keeps a worker (the library's poll, 10 ms [V: `worker.cpp`]) and a
separate notifier, so a late "played" report never waits behind the worker's disk writes (`worker.cpp:307-323`).

### 7.5 The program kind: prerequisites, not a design

An `airplay` type wrapping shairport-sync needs, before the program kind can be written:

- **Serving PipeWire's socket** (`core.daemon = false` today; AGENTS.md lists it as not built). Without it a child
  program cannot join the graph at all. The alternative, `program{argv, stdout → feed}`, needs no socket and is the
  smaller first step.
- **The mark** comes from the socket's peer credentials (`pipewire.sec.pid`), not from properties a program sets for
  itself; ear writes the program's configuration to give its nodes `<node>.<suffix>` names and to turn its autoconnect off.
- **Health is process-alive only** (design rule 1). The program's stream is an inside join that may come and go without
  being a loss; a stream that closes between sessions must not make recovery kill a healthy process [U: whether
  shairport keeps its stream open while idle].
- Claims for its ports (5000, 7000), and the mDNS resource, or its own Avahi use.

## 8. The control plane

### 8.1 Channels, by what they are

| Channel | Declared in | Kept | Written by | Delivery | On the wire | On standard output |
|---|---|---|---|---|---|---|
| **Settings** (desired) | `SettingsTable` | `ear.conf` | the graph file keeper | as today | `node.added`, `node.updated` | as today |
| **Parameters** (desired, live) | rows marked `live` | `ear.conf`, at commit (8.5) | the keeper | to an RT slot, wait-free | `node.param` | at commit only |
| **Status** (observed) | `StatusTable`, one writer per field: `loop`, `rt` or `resource` | memory; in snapshots | that writer | coalesced, rate-capped (8.2) | `node.status {name, fields}` | `node <n> status <field>=<value>` |
| **Events** (edges) | `EventTable` | not kept | the run's loop | queued, rate-limited (8.3) | `node.event {name, code, args}` | `node <n> says <code> <args>` |
| **Actions** (commands in) | `ActionTable` | not kept | a client, through the keeper | to the run's loop; reply | `node.act` → `{}` or an error | — |

Health (`node.state`) is not a channel a type writes (I2).

**One ordered stream per node.** Status, events and facts from one run reach the main loop in the order they were
written: before an event or a fact is delivered, the node's dirty status fields are flushed. So `connected="Kitchen"`
always precedes `paired`, and the last status before a `lost` is not overtaken by it. A snapshot carries each node's
status and state from the same main-loop turn.

### 8.2 Status: coalesced slots, capped rate

- Each field is a slot with exactly one declared writer, so a seqlock's single-writer rule holds. `rt` fields are
  fixed-size (flag, whole number, closed enum); text fields are written by a loop or a resource.
- A loop or resource write marks the field dirty and, if the node was clean, rings the status bell once (gated until the
  drain). An `rt` write never rings: `rt` fields are sampled at each drain.
- The main loop drains at most once per 33 ms (one level window): a bell rung sooner arms a one-shot timer for the rest
  of the window. So `node.status` is at most 30 a second per node, whatever a peer or the realtime thread does, and the
  drain is still driven by events (the bell), not polled.
- Counters the framework keeps for every node (frames a handoff dropped, frames a feed came up short) are not status
  writes: they are read at a drain, in snapshots and on the `stopped` line (`dropped=`), so a node idling on an empty feed
  makes no traffic.
- Text is the framework's to make safe: UTF-8 checked, control characters scrubbed, quoted on standard output, bounded
  (declared maximum, default 256 bytes, cut and marked). A peer's text (a server name) needs no per-type code (today
  `notes::printable`, `notes.hpp:14`).
- Coalescing drops intermediate values within one window: right for state, which is why edges are events (Q2).

### 8.3 Events: queued edges

Rare by declaration: `pairing-failed <reason>`, `key-write-failed <detail>`, `paired <server id>`. Codes and argument
kinds come from the table. A bounded per-node queue; a burst of 32 passes, then one a second, and what was held back is
counted in one `events-dropped <n>` (today's `NoteGate` policy, `note_gate.hpp`, moved into the framework). Not
writable from the realtime thread.

### 8.4 Actions: commands in

- Every control request already goes whole to the keeper's thread in arrival order (AGENTS.md, threads); actions do too,
  so an action is ordered with the edits around it. The keeper posts it to the main loop, the framework checks its
  arguments against the type's table and posts it to the run's loop; the handler's `std::expected<void, std::string>`
  becomes the reply.
- A queued action whose run ends before it runs gets `restarted` (a restart or recovery) or `stopping`; an action already
  running finishes and replies before the run's loop closes (I5 step 4). A node with no run answers `not-running`.
- **`restart` is every node's**, the framework's own: the same teardown and start as a settings change, without editing
  `ear.conf`, and it skips any recovery wait.
- An action changes no desired state. What should survive a restart is a setting or a parameter.

### 8.5 Parameters: one model for live values

Revision 1 had live settings, parameter slots and actions overlapping. They are one model now, after CLAP's parameters
and gestures:

- A setting row declared `live` is a **parameter**. The framework owns its realtime slot (wait-free: a triple buffer
  with an atomic index exchange, so the realtime reader never retries) and gives `process` its value as
  `io.param(gainRow)`. A type writes no slot, no `onSettings` and no loop for it, so a type with no loop has parameters.
- `node.param {name, key, value, gesture}` moves it: `begin`, `change`… set the slot and tell subscribers, persisting
  nothing; `end` (or a set without a gesture) commits once, as a settings edit at its source through the desired graph,
  and `ear.conf` is written behind as usual. A fader drag is one file write, not one per tick.
- A hand edit or `graphFile.set` that changes a live row's value moves the slot without a restart. A change to any
  other row restarts; a change of ports replaces.
- Smoothing is offered, not imposed: `Smoothed<float>` ramps a parameter over a declared time in `process`.
- A live row of an outside node's property (a card's volume) is set by its kind's adapter (7.3).
- Timestamped changes (a mute at a media time) need an event ring keyed on graph position; reserved, not designed (15).

### 8.6 Recovery (section 10 has the clock's part)

`NodeState` gains `recovering {retryInMs}`. The spec declares `recovery::none` (today's behaviour, the default) or
`recovery::backoff{first, max, attempts}`. After a loss, a node with backoff is torn down (I5), waits, and starts again;
each failure doubles the wait up to `max`, and a run that stays `running` for `max` resets the count. After `attempts`
failures in a row it is `failed`, and `restart` or a settings change starts it again. The wait is a main-loop timer.
A lost block is final (I1). This is also how a program that keeps crashing restarts (`pipewire-graph.md`: "backoff
sounds good").

### 8.7 The wire schema

- `types` stays a sorted list of names (`graph_view.hpp:25`, `test/protocol/snapshot.json`). A new field,
  `typeSchemas`, gives for each type its settings rows (key, kind, required, default, range, unit, live, what a text must
  be), banks, status fields, event codes and actions, generated from the spec, with a hash of each type's schema so a
  client can cache it.
- `graph.get` gains `protocol`, the framework's wire version, separate from the types' hashes.
- New notifications (`node.status`, `node.event`, `node.param`) are ignored by a client that does not know them; the
  editor today ignores events naming unknown nodes (`batch2.md` section 6), and the rule becomes "unknown methods too".
- The editor renders forms, status and action buttons from `typeSchemas`, naming no type in `web/`.

## 9. The audio contract

### 9.1 Bus connections: the type writes none

Public banks are declared; the framework makes the block, its ports, the routes' links and status, and the levels, as
today. An outside resource names the inside bank it joins, and the framework joins it. A pass-through is the framework's
code. A type writes no link, port id, registry lookup or route handling; `card` and `rtp` write no audio code.

### 9.2 `Io` by bank

```cpp
struct White {
	std::mt19937 gen{std::random_device{}()};
	void process(Io io, const GraphCycle&) noexcept { for (auto ch : io[noise]) fill(ch, gen); }  // `noise` from the spec
};
```

Bank handles resolve to indices in `prepare`, so `io[bank]` is an array index on the realtime thread. Each member is a
span of exactly `io.frames()`; an unlinked input reads zeros and every output sample is written (today's contract,
`node_type.hpp`).

### 9.3 `GraphCycle` grows the clock facts network nodes need

Today it carries `startNs`, `position` and `delayFrames`, from `spa_io_position.clock` (`port_blocks.cpp:197`). It
gains, from the same structure: the driver's identity (its clock id), `discontinuity` (set by the framework for the first
cycle after a driver change or a position jump), `rateDiff`, `nextNs` and the xrun count. A type dating what it played
(sendspin, `sendspin_code.cpp:52`) then knows when its clock jumped instead of hard-syncing blindly.

### 9.4 Latency is the framework's

The framework owns the `pw_filter`, so a type cannot set port latency. By default `pw_filter` merges latency across all
of a filter's ports, which mixes independent paths (a card's playback and capture; sendspin's `to` and `from`) [U: the
merge rule, from `pw_filter`'s source]. So:

- port blocks use `PW_FILTER_FLAG_CUSTOM_LATENCY` and the framework propagates latency per bank pair;
- `passThrough` forwards the outside node's latency onto its public bank;
- a type declares its own internal latency in the spec (static) or reports it from its loop (dynamic: `rtp`'s
  `latencyMs`, a feed's size);
- `process` sees, per output bank, the latency downstream of it, which replaces the single driver `delayFrames` (wrong
  today when sendspin's output reaches a following card or a file).

This is what R7's zones, sendspin's clock and an AirPlay delay estimate all need; it moves from revision 1's open items
into the contract, and has its own slice (section 13, slice 6).

### 9.5 Driving the graph

- **Invariant: only the dummy driver and sound cards drive.** Every network and program type follows the graph's clock
  (as `rtp` and `sendspin` do today). A driver resource kind (`node.driver`, `pw_filter_trigger_process`) is not
  designed; a type that needs one is a core change.
- **The graph-progress watchdog.** A frozen driving card stalls every block in its group without any state change (the
  MOTU's freezes, `ear-on-talos` skill). No level window within a bound (several windows) is a fact against the driving
  resource: the framework reports it lost, which hands the clock back to the dummy driver.
- A card's playback and capture are two ALSA adapters of one device; whether the following one resamples against its own
  clock unless they share a clock is [U] and checked in the card slice.

## 10. Recovery and the clock

A sound card drives by being linked into the blocks' graph group (`node.group = ear.graph`, `cardPriority = 1` below
the dummy's 200000, `engine_names.hpp:12` [V]). Each recovery attempt on the driving card moves the clock twice, for
every node: to the dummy at teardown, back to the card when its links go active. Each move jumps `position` and `nsec`
and changes `delay`, so every `rtp` receiver re-converges (about `latencyMs` of audio lost), sendspin hard-syncs
(`sendspin_code.cpp:26`), and a recording gets a discontinuity. With two cards, the other takes the clock and may lose it
again: a third move. So:

- **Driver choice becomes an explicit policy**, not a side effect of link order (Q3): one designated clock card, or a
  per-card priority setting. A card that is not the clock never takes it, in recovery or otherwise [U: how, in PipeWire;
  the card slice's spike].
- **The driving card's recovery is capped** (`attempts`), then `failed`, and its cost is stated on the wire with each
  attempt (`node.state recovering {retryInMs, drivesClock: true}`).
- **Recovery for followers** (non-driving cards, `rtp`, `sendspin`) costs only their own audio, and may use backoff.
- `discontinuity` in `GraphCycle` (9.3) lets every type handle the moves it cannot avoid.

## 11. The types, rewritten

| Type | Today | This proposal (estimate) | What remains its own |
|---|---|---|---|
| `white` | 73 lines, 4 classes | about 25, 1 struct | the noise loop |
| `card` | 105, 4 classes | about 40 | its rows, its `check`, two acquisitions, `passThrough` |
| `rtp` | 114, 4 classes | about 45 | its rows, one acquisition, `passThrough`; `receiving` comes from its adapter |
| `file` | 190 | about 90 | its rows, the path rule, libsndfile in a `HandoffConsumer` |
| `sendspin` | about 1,900 | about 1,300 | the protocol, pairing, the time filter, PCM, mDNS (until a second user); status for connection, code, stream, sending; events for pairing results |
| `gain` (new; needs `decimal` and `live`) | — | about 30 | one live row, `Smoothed<float>`, a multiply |

```cpp
// sendspin's start, in this proposal
static Started start(Context& cx, const Checked& s) {
	if (!s.get<serverRow>()) cx.acquire(claim::tcpListen{s.get<portRow>()});   // waits or refuses per I8
	auto& feed = cx.acquire(feed{from, feedFrames(cx.format())});
	auto& worker = cx.acquire(loop::dedicated);
	auto& notifier = cx.acquire(loop::dedicated);
	auto client = SendspinRun::make(cx, worker, notifier, cx.store<keyStore>(), feed, s);  // owns the library's threads
	auto& toServer = cx.acquire(handoff{to, cx.format().rate / 2, client->toServer()});
	return Started{SendspinCode{feed, toServer, client->progress()}, std::move(client)};
}
// Teardown (I5): barrier, release; handoff `to` drains into the client; the client closes, stopping the library's sync
// thread (the feed's writer) and both loops; then the feed, the store and the claim go.
```

## 12. What it gives up, and costs

- **More compile-time machinery.** The settings table already needs 37 compile-fail tests (about 50 s at one job).
  Status, event, action and store tables reuse its rows, so they add cases, not a second machine.
- **A wider core.** `ear_core` gains the resource contracts, the per-node ordered stream, parameter slots and two
  main-loop timers (status cap, recovery) plus readiness deadlines. It stays free of PipeWire, sockets, threads and files:
  the loop pool, eventfds and stores are behind interfaces in `ear_pipewire` and `ear_folder`.
- **Status lines change.** Sendspin's state-like `says` lines become `status` lines; the system tests and AGENTS.md's
  status-line contract change.
- **Coalescing loses intermediate states** within a window, by design.
- **A shared strand pool** is a new place where one node can delay another; `dedicated` is the escape.
- **Resource kinds stay a closed list in core.** Only types are open.
- **The watchdog can misjudge** a host so loaded the data thread stalls (`rtp.md` erratum 3); its bound must sit well
  above the xrun stalls the tests already tolerate.

## 13. Slices

Each slice keeps every existing test green and changes no audio path unless it says so.

1. **The registry and the teardown contract.** `Context::acquire` with the two outside kinds and handoffs; named inside
   banks; `passThrough`; I4 to I7 as tests (a feed writer blocked at teardown, a loop mid-handler, a late status write
   from an old generation). `NodeHost` takes the registry. `card` and `rtp` move.
2. **The SPI's shape.** `TypeSpec`, `check`, bank constants, `NODE_TYPE`, `Io` by bank; `white` and `file` move; folders
   become resources.
3. **Primitives into core.** Loops, feed, mailbox, wake, stores (boot sweep, async open), claims (I8). `sendspin` moves
   and deletes its copies.
4. **The control plane.** The ordered per-node stream, status, events, actions through the keeper, `restart`,
   `typeSchemas` and `protocol`, the editor rendering from them; sendspin's notes split.
5. **Parameters.** `decimal` and indexed rows, `live`, triple-buffered slots, `node.param` with gestures,
   `Smoothed<float>`; a `gain` type proves it.
6. **Clock and latency.** `GraphCycle`'s new facts, custom latency per bank pair, declared and reported latency,
   downstream latency in `process`.
7. **Recovery and driver policy** (its own design page first, with a PipeWire spike): readiness deadlines, the watchdog,
   adapter-sourced status (`rtp` `receiving`), the clock policy, `recovering` with capped backoff.
8. **Programs**: after a decision on serving the socket (7.5).

## 14. Gate questions for the owner

| # | Question | Options |
|---|---|---|
| Q1 | Do status lines replace the state-like `says` lines on standard output? | Yes; `says` is for events (recommended) / keep both |
| Q2 | Status coalesces within a 33 ms window, dropping intermediate values. Accept? | Accept (recommended) / queue every change |
| Q3 | How is the clock card chosen? | One designated clock card, a graph-level setting (recommended) / a per-card priority setting / as today, whoever links |
| Q4 | Default recovery | Driving card: `none` until Q3 is built, then capped backoff; followers: `backoff{1s, 60s, 10}` (recommended) / `none` everywhere |
| Q5 | Loops | Strands by default, `dedicated` on request (recommended) / a thread for each |
| Q6 | Live values | Parameters with gestures, committed to `ear.conf` at gesture end (recommended) / every change written / live values never written |
| Q7 | Programs' first step | `program{argv, stdout → feed}`, no socket (recommended) / serve PipeWire's socket first |

## 15. Open items

- [U] Whether PipeWire's data thread flushes denormals for filter callbacks; if not, whether the framework sets FTZ/DAZ
  around `process`.
- [U] `pw_filter`'s default latency merge, and `PW_FILTER_FLAG_CUSTOM_LATENCY`'s behaviour in 1.4.2.
- [U] How a card is kept from driving (Q3), and whether a card's playback and capture adapters share a clock.
- [U] Whether a USB unplug reaches the ALSA adapter's ERROR state without udev in the container.
- [U] `cleanup.sec`'s smallest safe value for `rtp`'s `receiving`.
- Stop costs one blocking barrier per node (`node_host.cpp`'s `stopAll`); batching them is a later improvement.
- Timestamped parameter changes and events (8.5): reserved.
- A driver resource kind (9.5): not designed.

## 16. Review record

Revision 1 (commit `docs(design): propose the node framework's primitives…`) went to three reviewers working
independently, each given the page and the code, not the author's reasoning: **A**, realtime safety and concurrency;
**B**, audio graphs, clocking and PipeWire; **C**, plugin-API design (CLAP, VST3, LV2, JUCE) and domain-driven design.
Every finding's evidence in the code was checked before it was taken. Disposition: **taken** (the section that answers
it), **partly**, or **not taken** (why).

### Blockers

| # | Finding | By | Disposition |
|---|---|---|---|
| 1 | Reverse-acquisition teardown frees the feed, handoff, store and claim while the run's threads (the library's sync thread writes the feed, `worker.cpp:123-135`) still use them | A, C | Taken: I5, dependency order. B's "keep reverse order" is overruled by A and C's evidence |
| 2 | The asynchronous close (`NodeInsides::close(closed)`, handoff close) is dropped; a blocking loop would hang the main loop, and `fail()` could join its own thread | A, C | Taken: I2 (facts cross to the main loop), I5 (every wait asynchronous) |
| 3 | Recovery ignores which card drives: each attempt moves the clock twice for every node | B | Taken: section 10, Q3, Q4; slice 7 has its own page first |

### Major

| # | Finding | By | Disposition |
|---|---|---|---|
| 4 | Param slot on a seqlock: a realtime reader can spin behind a preempted non-realtime writer | A | Taken: triple buffer (8.5); no retry loops on RT (7.4) |
| 5 | RT-written counters ring the bell every cycle: about 190 wakes and wire events a second per idle sendspin node | A | Taken: RT never rings; counters sampled; 33 ms cap (8.2) |
| 6 | Stale status from an old run, writes after removal, two writers per field | A | Taken: I6 (generation, shared handle), one writer per field (8.2) |
| 7 | Sendspin's threads misread: the feed's writer is the library's thread; the notifier is separate on purpose; feeds would block strands | A | Taken: run object owns library threads (7.2), several dedicated loops, feed writes only off strands (7.3, 7.4) |
| 8 | Store reads moved onto the main loop; boot sweep of orphaned key files lost; type builds the file name | C | Taken: stores declared by suffix, opened off the main loop, sweep is the framework's (7.1, I9) |
| 9 | Rules across settings had no home; moving card's to `start` would write refused settings to `ear.conf` | C | Taken: `check` at parse (7.1) |
| 10 | Live settings, param slots and actions overlap; no fader model | C | Taken: one parameter model with gestures (8.5), Q6 |
| 11 | A gain or mixer cannot be written: no decimal kind, no indexed rows | C | Taken: prerequisites in 7.1, slice 5, a `gain` estimate in 11 |
| 12 | The program kind is incoherent: no socket, the mark, configuration, idle streams | B, C | Taken: 7.5 as prerequisites, Q7 |
| 13 | Status, events and health had no order between them | C | Taken: one ordered stream per node (8.1) |
| 14 | `types` already exists as a string list; one protocol number mixes framework and types | C | Taken: `typeSchemas` with hashes, `protocol` separate (8.7) |
| 15 | Actions bypassed the keeper; in-flight actions at restart unspecified | A, C | Taken: through the keeper; `restarted`/`stopping`; in-flight finish (8.4, I5) |
| 16 | `s[row]` and bank checks against `ports()` cannot be compile-time | C | Taken: `get<row>()` kept; bank names constants in the spec (7.1) |
| 17 | "Never ready, never lost" (rtp port held elsewhere, a card that never runs, a frozen driver) | B | Taken: readiness deadlines (7.3, I3), watchdog (9.5) |
| 18 | `rtp`'s `receiving` cannot be measured by a type with no code; it lives on the node proxy | B | Taken: adapter-sourced status (7.3) |
| 19 | `GraphCycle` drops clock identity, discontinuity, rate and xruns | B | Taken: 9.3 |
| 20 | Latency must be the framework's now; `pw_filter` merges latency across ports | B, C | Taken: 9.4 and slice 6; the merge rule stays [U] |

### Minor and nits

| # | Finding | By | Disposition |
|---|---|---|---|
| 21 | Wake gating unspecified; sendspin rings every cycle today | A | Taken: "at most once until answered" in 7.3 |
| 22 | Teardown does not always start with `drive(nullptr)`; stop costs a barrier per node | A, C | Taken: I4 reworded; batching in 15 |
| 23 | Live settings on a type with no loop | A, B | Taken: parameter slots need no loop (8.5) |
| 24 | Name reuse (`olderGoing`) must cover claims and recovery; a remove and add of one port in one plan | A, C | Taken: I8 |
| 25 | Peer text on status lines needs framework sanitising | C | Taken: 8.2 |
| 26 | Thread rules need assertions | C | Taken: 7.4 |
| 27 | `NODE_TYPE` must not self-register; keep the folder check | C | Taken: 7.2 |
| 28 | `NodeSpec` is taken | C | Taken: `TypeSpec` |
| 29 | DDD decoration: claims are cross-node; the class diagram's inheritance contradicts the variant; labels without consequence | C | Taken: claims as a domain service (5, I8); diagram redrawn; labels dropped |
| 30 | Sample-accurate timed changes | B | Partly: reserved in 8.5 and 15, not designed |
| 31 | A node that wants to drive | B | Taken as an invariant (9.5); a driver kind not designed |
| 32 | Card playback and capture adapters' clocks | B | Open item [U] (15), checked in the card slice |
| 33 | Outside nodes' live properties (card volume) | B | Partly: the adapter's, through a parameter (7.3, 8.5); not designed |
| 34 | A lost block and recovery | B | Taken: I1 |
| 35 | The level bell is drained on the control thread, so the status bell is new design, not a reuse | B | Taken: 8.2 no longer claims reuse |
| 36 | "Restarting a failed node takes a no-op settings edit" was wrong | A, C | Taken: section 2 corrected |

Not taken: none. One conflict between reviewers (row 1) was decided on the evidence.
