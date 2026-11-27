# Architecture

EAR ("Einstein Audio Router") is one Linux process that runs PipeWire's audio engine inside
itself and builds an audio graph from a text file, `ear.conf`. It starts the nodes the file
names, links their ports as the file's routes say, and keeps the file live: an edit made through
the control port (port 7777, the graph editor or a script) is applied to the running graph and
written back into `ear.conf`, and a hand edit to `ear.conf` is applied and shown in the editor.
Every change is printed on standard output as one JSON record.

This page is the map: what the parts are, which thread runs what, how a change and audio move,
and what a node's life looks like. The details live on their own pages:
[graph-file.md](graph-file.md), [node-types.md](node-types.md),
[writing-a-node-type.md](writing-a-node-type.md), [control-port.md](control-port.md),
[output-records.md](output-records.md), [engine.md](engine.md) and [editor.md](editor.md).

## The ideas it is built on

- **The graph always runs.** Every port block processes every cycle whether it is routed or
  not, and nothing is suspended. A node starting, stopping, failing or being reconfigured
  changes only that node's own audio; its block, its ports and its wires stay, and a peer wired
  to a silent node reads zeros. *Why:* the owner's rule, "in a general purpose
  sources-and-sinks graph, its always just 'running'".
- **One clock, ear's own.** The graph is always driven by ear's internal clock (PipeWire's dummy
  driver, tracking `CLOCK_MONOTONIC_RAW`). Sound cards, RTP receivers and network nodes follow
  it and never drive it, so losing a card never moves the clock under anything else
  ([engine.md](engine.md#the-internal-clock)).
- **One source of truth for what is wanted: `ear.conf`.** The control port has no state of its
  own; every graph edit is an edit of the file, judged before it is kept
  ([graph-file.md](graph-file.md)).
- **Health is the framework's.** A node type reports facts (a resource lost, a note); the node
  framework alone decides a node's state.
- **PipeWire does the audio; ear does the policy.** PipeWire owns the clock, rate matching,
  resampling, devices and network audio: every rate conversion is PipeWire's resampler, in a node
  at the graph's edge. ear decides which nodes exist and which links join them, and a node that
  brings in a clock of its own decides how that clock meets the graph's tick
  ([the timing model](#the-timing-model)). There is no PipeWire daemon, no socket and no session
  manager.
- **Every rule has one home, and a type holds it where it can.** A rule the engine and the tests
  both need is one pure function in `ear_core` that each calls, never a copy: the latency model
  ([engine.md](engine.md#latency-per-bank-pair)), dating by `FrameClock`
  ([engine.md](engine.md#dating)), the sink's, the source's and the block's rules the type
  harness shares ([writing-a-node-type.md](writing-a-node-type.md#testing-a-type)). Where a rule
  can be a type, it is one rather than a guard reading source text: a rate is a `SampleRate`, a
  PipeWire property key a `PropertyKey` of its home ([engine.md](engine.md#properties-ear-sets)),
  a C callback a `Guarded` value ([below](#the-callback-boundary)), a test's sleep a role call
  ([testing.md](testing.md#sleeps-by-role)). *Why:* the owner's rule (2026-11-09), "always patch
  the root class of a bug, not the bug itself. Clusters of issues are a smell that signals an
  architecture problem": slice 3's reviews found the same bug fixed in two places (the block and
  the harness), one callback at a time, one sleep at a time, and each class was closed by giving
  its rule one home.

## Packages

Each C++ package lives in `src/<package>/` with its own `CMakeLists.txt`, public headers under
`src/<package>/include/ear/<package>/` and tests in `tests/`. A node type lives in
`src/nodes/<type>/` and is its own library ([writing-a-node-type.md](writing-a-node-type.md)).

| Target | Owns | Never |
|---|---|---|
| `ear_core` (`src/core`) | The desired graph (`DesiredGraph`: the one judge of every change, `plan` then `commit`); the node type contract (`NodeType`, settings tables, `TypeSpec`, the type's class and its contexts); the node framework (`NodeHost`, the node lifecycle machines, health, `NodeOperations`, the pass-through, companions, latency plans, the claim registry); the reconciler (route links and route status); levels; the graph read (`GraphStatus`); `FrameRing`. Beside it, for tests only, `ear_type_harness` (`ear/testing/`) | Any PipeWire call, socket, thread or file access. nlohmann/json is its only dependency |
| `ear_runtime` (`src/runtime`) | The threads the node framework runs work on, behind `ear_core`'s interfaces: the I/O thread (`IoThread`, core's `IoExecutor`), the one thread for blocking work; the nodes' loops (`Loops`, core's `LoopAdapter`: the strand pool and each dedicated loop's thread) ([threads](#threads)); and the nodes' stores on disk (`ConfinedStores`, core's `StoreAdapter`, over the graph file folder) with the boot sweep of every declared store ([writing-a-node-type.md](writing-a-node-type.md#stores)); and the kernel's word on a port a claim names (`KernelPortProbe`, core's `PortProbe`, [writing-a-node-type.md](writing-a-node-type.md#claims)); and the processes under the nodes' programs (`Programs`, core's `ProgramAdapter`, on the `ear-programs` thread, which also reaps every orphan ear inherits: [writing-a-node-type.md](writing-a-node-type.md#programs)) | Policy (what a job means is its caller's; staging, a loop's state and the sources it left, when a store is narrowed, trusted or written, and who holds a claim, are `NodeHost`'s); PipeWire. No node type links it |
| `ear_folder` (`src/folder`) | `ConfinedFolder`: a folder held open from boot, every path resolved beneath it by the kernel (`openat2` with `RESOLVE_BENEATH`); files replaced only if unchanged (`renameat2(RENAME_EXCHANGE)`); `requireApart`, the boot check that no two folders overlap | Following `..`, absolute paths or (for the graph file) any symlink |
| `ear_node_white`, `ear_node_gain`, `ear_node_file`, `ear_node_card`, `ear_node_rtp`, `ear_node_sendspin`, `ear_node_airplay` (`src/nodes/<type>`) | One built-in node type each ([node-types.md](node-types.md)) | Another node type; PipeWire or the framework's threads (only `ear_core`'s interfaces: no type links `ear_pipewire`, `ear_runtime`, `ear_control`, `ear_config` or `ear_server`) |
| `ear_all_node_types` (`src/all_node_types`) | `allNodeTypes()`, the one list of node types, generated at configure from the folders in `src/nodes/` | Naming a type by hand |
| `ear_pipewire` (`src/pipewire`) | The in-process engine and the adapters that implement `ear_core`'s interfaces: port blocks, links, live ports, sinks, sources, engine companions (sound cards, module-run converters: RTP receivers and pipe receivers), the main-loop executor, the engine's log ([engine.md](engine.md)); the callback boundary every C callback ear gives PipeWire goes through ([below](#the-callback-boundary)) | Policy; a C callback registered outside the boundary |
| `ear_mdns` (`src/mdns`) | A node's mDNS listing (`Advertiser`, its `Listing` and `listingRefused`): one DNS-SD service instance probed, renamed on a clash, announced, answered and said goodbye to over its own socket on port 5353, run on whichever loop of the node type keeps it ([node-types.md](node-types.md#mdns)); mjansson/mdns, privately. Its waits (RFC 6762's protocol time) are a timerfd of its own behind its descriptor, an epoll its owner's loop watches | A thread; anything of ear's |
| `ear_output` (`src/output`) | Where standard output goes: the output thread, the capture of descriptors 1 and 2, the main-loop watchdog ([output-records.md](output-records.md)) | A record's text |
| `ear_control` (`src/control`) | Standard output's one encoder (`StatusLog`); the JSON-RPC protocol (envelope, decoding, commands, events, sessions); the wire types of the graph file and the lifecycle; `MainLoopGateway`, the one crossing between the main loop and the control thread | Sockets; graph rules; knobly |
| `ear_config` (`src/config`) | The graph file keeper: `ear.conf`'s model through knobly, the keeper's thread (working copy, folder watch, writes) and its main-loop side (plan and commit, the running version) | Judging a graph; letting a knobly type leave its thread |
| `ear_server` (`src/server`) | The control thread and port 7777 (Boost.Beast): the editor's files, `/rpc`, Host and Origin checks, per-client queues, the level bell | Graph state; PipeWire |
| `ear` (`src/main.cpp`) | The composition root: wiring, boot and stop order, and glue between packages that do not see each other | Computing anything |
| `web/` | The graph editor (React, React Flow, Vite; [editor.md](editor.md)) | Graph rules: it draws only what ear announced and never parses knobly |

**Dependency rules.** Dependencies point one way: `ear_core` is below everything; the node types,
`ear_runtime`, `ear_pipewire`, `ear_control` and `ear_config` depend on it; `ear_control` is below
`ear_server` and `ear_config`; `ear_folder` is below `ear_runtime`, `ear_node_sendspin` and
`ear_config`; `ear_mdns` is below the node types that keep a listing (`ear_node_sendspin`, `ear_node_airplay`); `ear`
links all of them, and links the node types only through `ear_all_node_types`. No node type links
another, nor `ear_runtime`, `ear_pipewire`, `ear_control`, `ear_config` or `ear_server` (configure
refuses each, `cmake/NodeType.cmake`); a node type may link `ear_mdns`, which is no node type.
`ear_output`, `ear_folder` and `ear_mdns` depend on nothing of ear's. No Boost,
knobly or sendspin-cpp type appears in a public header.

**An interface lives with the package that consumes it.** `node_host.hpp` declares
`PortBlockFactory` and the resource adapters, `reconciler.hpp` declares `LinkMaker` and the
live-port and link listeners, and `ear_pipewire` implements them; `requests.hpp` (in
`ear_control`) declares `GraphRequests` and `GraphFileListener`, which `ear_config` implements
and consumes. Each package's headers form a CMake header set compiled on its own
(`VERIFY_INTERFACE_HEADER_SETS`), and `ear_pipewire`'s headers only forward-declare PipeWire's
types, so PipeWire's headers stay private to it.

## Threads

*Why so many:* the owner's rule, "the control port shouldn't run in the PipeWire context,
because it will not be real time". The realtime thread does only audio; the main loop holds all
graph state; everything that can block (sockets, disk, libraries) runs elsewhere and crosses to
the main loop by posting.

| Thread | Runs |
|---|---|
| PipeWire's data loop (realtime, one for the whole engine) | Every port block's `process`, level metering, the sources' plays and the sinks' taps; every engine companion's processing |
| PipeWire's main loop (the main thread) | Every piece of graph state: the engine, desired graph, node framework, reconciler, adapters, the gateway's graph side, the keeper's main-loop side; `ear.conf`'s first read at boot |
| The control thread | Port 7777: sockets, JSON-RPC, sessions, queues. It holds only the gateway, which sends every request whole to the keeper and gets replies and events back in order from the main loop |
| The keeper thread | The graph file keeper: every request, disk check and write, one step at a time; the working copy (knobly's object, which never leaves this thread); the inotify watch. It waits in `poll()` with no timeout |
| The I/O thread (`ear-io`) | The framework's blocking work, off the main loop and every node's threads, one job at a time in post order (`IoThread`, `ear_runtime`). A node's stores are opened and written there ([writing-a-node-type.md](writing-a-node-type.md#stores)), and its recordings opened and closed ([writing-a-node-type.md](writing-a-node-type.md#recordings)). A job holds by `shared_ptr` what it fills and reports through its configuration's channel ([teardown](#teardown-by-dependency)), on the main loop, so one whose configuration ended reports nothing and the handle to what it opened goes back to this thread to be released, never closed on the main loop. It is one thread for all of ear: a hung disk stalls every open and every store write, which the 5 s bounds on each make visible as the nodes' failures |
| A node's loops ([writing-a-node-type.md](writing-a-node-type.md#loops)) | A type's work off the realtime thread and the main loop, each loop with its state, one handler at a time per loop (`Loops`, `ear_runtime`). A dedicated loop has a thread of its own, `ear-<name> <node>` cut to 15 bytes, and may block; strands share a pool started with the first strand: `min(4, hardware threads)` workers (`ear-strand`), one more for a handler stuck past 1 s (at most 16 in all), and one reactor thread (`ear-strands`) that turns the strands' timers and watches into work and watches the workers. Timers on `CLOCK_MONOTONIC`, watches on epoll; each loop's events reach the main loop by posting. A `sendspin` node has two dedicated loops, `ear-sendspin <node>` (its worker) and `ear-notify <node>` (its notifier) |
| The programs' thread (`ear-programs`) | Every node's program (`Programs`, `ear_runtime`), started by the first launch: it writes a program's files and makes its feed, spawns it, tells its output's lines, its feed's bytes and its end, stops it (SIGTERM to its group, SIGKILL past 3 s) and reaps every child ear has, a program or an orphan re-parented to ear ([boot](#boot)). It waits in epoll on each program's pidfd, pipes and feed and on a signalfd for SIGCHLD; its one timer is the SIGKILL a stop owes, a timerfd. What it tells crosses to the main loop through the configuration's channel ([writing-a-node-type.md](writing-a-node-type.md#programs)) |
| Sink threads | A `file` node's sink (its WAV writer); a `sendspin` node's sink to the server (`write_audio`) |
| The output thread | Every write to the real standard output, from a bounded queue |
| The capture thread | Reads the pipes on descriptors 1 and 2 into `log` records, and drains the engine's log ring |
| The watchdog thread | `StallWatch`: watches the main loop's heartbeat ([below](#bounds-the-wedged-data-loop-and-the-watchdog)) |
| A `sendspin` node's library | The protocol library's own threads (protocol, sync, source, IXWebSocket's), started and joined by its worker loop's state ([node-types.md](node-types.md#threads)) |

Everything is moved by events, not polling, with one exception: a `sendspin` node's worker loop
turns its library on a 10 ms timer and watches its connection with 5 s and 31 s timers, because the
library has no wake hook and no event for a lost connection or a failed dial. Those timers stay on
that loop.

## The callback boundary

PipeWire and alsa-lib call ear through C function pointers: loop events and timers, the stop
signals, invokes, the listeners of filters, proxies, nodes, links, the registry, the core and
modules, PipeWire's log and alsa-lib's error handler. An exception that reached one would unwind
through C frames, which is `std::terminate` with no word of where. So every C function ear hands
either library is registered through one boundary, `src/pipewire/callback_boundary.hpp` (private
to `ear_pipewire`), with no exception:

- **`guarded<&Class::member>`** is a `noexcept` trampoline that calls the member and catches. What
  it caught goes to the `Faults` the engine was made with (`ear/pipewire/faults.hpp`,
  `EngineSettings::faults`), naming the member (`FilterPortBlock::onStateChanged`) and where the
  main loop was when it was thrown (below), and then to the object itself when its class is a
  `FaultOwner`: an adapter whose state is one node's ([below](#faults-by-owner)).
- **`realtime<&Class::member>`** is for the data loop: a filter's `process`, a block's request
  invoke and its blocking flush, the engine's denormal and scheduling invokes, and the internal
  clock's `start` and `incomplete` events. The member must
  be `noexcept` and nothing is added around it: realtime code never throws, and a throw there is
  `std::terminate` ([engine.md](engine.md#the-one-data-loop-and-invokes)).
- **`realtimeFunction<&function>`** is for the plain C functions a library calls on any thread, the
  data loop among them, with C varargs no trampoline can forward: the engine log's methods
  (`installLog<LogCallbacks{...}>`, PipeWire's process-wide log) and alsa-lib's error handler
  (`setAlsaErrorHandler`). The function must be `noexcept`; nothing is added.
- **Registering** is only through `addEvent`, `addTimer`, `addSignal`, `invoke`, one `listen` per
  events table (`FilterCallbacks`, `ProxyCallbacks`, `NodeCallbacks`, `LinkCallbacks`,
  `RegistryCallbacks`, `CoreCallbacks`, `ModuleCallbacks`), `listenRealtime` for an engine node's
  realtime events (`NodeRealtimeCallbacks`, `realtime<>` entries only: the internal clock's cuts,
  [engine.md](engine.md#graphcycle)), `installLog` and `setAlsaErrorHandler`,
  which take only those values, and the loop's kind is a type rule: event, timer and signal sources
  (on loops that may run anything: the main loop, a sink's thread loop) and an invoke
  on any `pw_loop` take only `guarded<>`; an invoke on the data loop takes a `DataLoop` (made from
  the context's `pw_data_loop` or a filter's) and only `realtime<>`; a filter's `process` entry
  takes only `realtime<>` and every other table entry only `guarded<>`. Each registration fills a
  handle its owner holds, which removes it when destroyed ([below](#faults-by-owner)); a realtime
  listener (`RealtimeListener<Target>`) holds the target its callbacks reach as well as its hook, and
  is removed by a blocking invoke on the data loop, so on a wedged loop the hook is left on its node
  and the boundary keeps hook and target for the process's life: its owner may go at once and cannot
  forget to leak what the loop reaches, and a listener that left one is never attached again. The engine finds one of its own nodes
  by name through the boundary too (`engineNode`), whose walk of the context's globals calls a
  function of the boundary's own. A raw function pointer, a
  method that can throw, or an object of another class than the member's does not compile
  (`src/pipewire/tests/compile_fail/callback_boundary_bad.cpp` holds each refused case).
- **The poison.** Nothing but the boundary can hand a library a callback, by the compiler:
  `src/pipewire/no_raw_callbacks.hpp`, forced into every source of `ear_pipewire` (`-include`,
  `ear_poison_raw_callbacks` in `cmake/RawCallbacks.cmake`), reads every PipeWire and SPA header
  the package includes and `callback_boundary.hpp`, then poisons (`#pragma GCC poison`) each way to
  hand one over: every loop source and hook (`pw_loop_add_*`, `spa_loop_utils_add_*`,
  `spa_loop_add_source`, `spa_loop_control_add_hook`), invoke and locked call (`pw_loop_invoke`,
  `pw_data_loop_invoke`, `spa_loop_invoke`, `pw_loop_locked`, `spa_loop_locked`,
  `pw_work_queue_add`), listener (every `*_add_listener`, `*_add_object_listener` and
  `*_add_rt_listener` PipeWire 1.4.2 declares, `pw_impl_node_remove_rt_listener` and the
  `spa_hook_list_*` and `spa_hook_remove` calls), stream or filter made with its events
  (`pw_stream_new_simple`, `pw_filter_new_simple`), interface or callbacks table made or handed
  over (`SPA_INTERFACE_INIT`, `SPA_CALLBACKS_INIT`, every `*_methods` table, `pw_log_set`, the
  `*_set_implementation` and `*_set_callbacks` calls, `pw_global_new`, `*_thread_utils_create`),
  walk that calls back on the calling thread (`pw_context_for_each_global`, the `*_for_each_*`
  and `*_match_rules` calls), `snd_lib_error_set_handler`, `dlsym` and `dlvsym`; and the types no
  registration can be made without (`spa_hook`, `spa_hook_list`, `spa_source`, `spa_interface`,
  `spa_callbacks`), so a listener PipeWire adds later is refused too. GCC refuses a poisoned name
  lexed after the pragma however it is spelt (split across lines, pasted by `##`, spliced by a
  backslash-newline, inside a macro defined after it) and leaves alone what was lexed before it:
  the boundary's templates and inline functions, which make every registration ear has, its
  `.cpp` making none, and PipeWire's own inline code. A PipeWire header read after the pragma that
  names a poisoned identifier fails, so one the forced header lacks fails closed until it is added
  there. Configure (`ear_refuse_raw_pipewire`, at its end) refuses any other target that sees
  PipeWire's headers (links `PkgConfig::PIPEWIRE` directly, through an interface library or an
  alias or inside a generator expression, or names its include directories; a static library's
  private link gives none) without the header forced in, but the two test programs that drive
  PipeWire themselves, `ear_pipewire_tests` and `ear_node_sendspin_tests`, named in
  `src/pipewire/CMakeLists.txt`. What neither holds: a macro defined before the pragma (a target's
  `-D`, written in CMake) expands past it, and another package could reach a PipeWire function
  through `dlsym` by a string, writing its C signature by hand; and a callback API a PipeWire bump
  adds under a new name, needing none of the poisoned types, passes until it is listed (the
  pipewire-engine skill's bump steps). The refusals are tested
  (`src/pipewire/tests/compile_fail/no_raw_callbacks_bad.cpp`, every poisoned name in one compile
  through `every_name.cmake`, and `test/pipewire/poison_refusal/` for configure's).
- **The main-loop executor** runs each posted item through the boundary on its own (named
  `MainLoopExecutor's posted work`), and each of its timers is a guarded callback
  (`LoopTimer::onTimeout`), so an item that throws is reported as itself and never drops the items
  posted behind it.
- **One engine at a time.** A trampoline carries only its object's pointer, so the `Faults` it
  reports to is the process's: `Engine::create` installs its engine's and fails (`another engine is
  running in this process`) while another engine holds the boundary, rather than let a second
  engine take the first's reports or remove them when it goes.

**Where it was thrown.** A fault caught on the main thread names what the main loop was in:
`MainLoopActivity`'s call and node (`configure hall`). Every scope has unwound by the time the
boundary catches, so a scope that a throw ends keeps its place (the innermost one's) for the
boundary to take. A scope that begins or returns, and a main-loop callback that begins, forget a
place left by a throw that was handled, but not while an exception is unwinding: a scope or a
callback that a destructor runs during the unwinding (`pw_filter_destroy` telling a state change)
leaves the place the exception carries. NodeHost, which runs every queued input and flush item
before it rethrows the first exception, keeps that exception's place and puts it back as it
rethrows. A callback PipeWire makes inside an open call (a state change inside `destroy block`) is
placed in that call. Off the main thread (a sink's thread) only the callback
is named.

**What follows** depends on who owns the fault ([below](#faults-by-owner)). `main.cpp`'s
`Faults` first writes an `ear.internal-error` record
([output-records.md](output-records.md#diagnostic)). A fault an adapter owns then returns and the
adapter fails its node. Any other ends the process, as it did before the boundary, but saying
where: the capture writes out what is queued (the record among it) and what its pipes hold, then
`ear: internal error in <callback>[, during <where>]: <detail>` whole on the real standard error
(`DescriptorCapture::lastWords`, never through a pipe), then `std::terminate` runs, whose own
message follows ([output-records.md](output-records.md#how-output-is-written)). An engine made
without a `Faults` (the tests') writes the same line to standard error and ends the same way for
every fault, owned or not.

**A node type's throw never reaches it.** NodeHost calls every piece of a type's code through one
catch of its own, before any boundary: its hooks and its code's `prepare`
on the main loop, and a sink's consumer on that one's thread. What a type
throws there, a `std::logic_error` included, is a fact about its node
([writing-a-node-type.md](writing-a-node-type.md#rules-the-framework-enforces)), and the rest of the
graph runs on. A type's code run at parse (its settings' parse functions, `check`, `ports`) goes
through the same catch in `NodeType::parse`, and what it throws refuses the edit as
`invalid-settings`. So a throw that leaves NodeHost, or a sink's own code, for the
boundary is ear's own bug, never a type's.

### Faults by owner

ear's own bugs are split by who owns the state the bug may have left half-changed (the owner's
answer, 2026-11-09: "split by owner", over ending the process for every bug or failing only a
node for every bug). *Why:* an
adapter's state dies with its node, so failing only that node is clean and the rest of the graph
plays on; NodeHost's state is every node's, and its invariants cannot be trusted after an exception
in the middle of an update, so ending the process (the container restarts it) stays the safe
answer there. Every caught bug is said in an `ear.internal-error` record (`callback`, `node` when
it fails one, `fatal` when ear ends after it, `detail`;
[output-records.md](output-records.md#diagnostic)), once per owner: an owner whose callbacks keep
throwing is said and told only the first time (`FaultOwner::firstFault`).

| Where it was thrown | Owner | What follows |
|---|---|---|
| A type's code: its hooks, its code's `prepare`, a sink's consumer, its parse | The type | Its node's fact (`start-failed`, or `lost` once rested; an edit refused at parse) and no record ([writing-a-node-type.md](writing-a-node-type.md#rules-the-framework-enforces)) |
| An adapter's own callback: a port block's, an engine companion's or its body's (a card's, an RTP receiver's), a link's | The adapter (`FaultOwner`) | The record, with the node; the adapter gives up what it holds and the node fails `lost`, `internal error in <callback>: <what>`. A block is lost for good (`blockLost`) and leaves the graph at once (its listener, the process callback with it, then, outside the filter's events, the filter itself), so every request its node's teardown makes is applied on the main thread and completes, and no silence can be judged a wedged data loop; an engine companion is torn down as on its loss, from wherever its callback left it, and a retry makes it afresh. A route's link fails its route, `refused` with that detail, and names no node |
| An adapter NodeHost calls inside a type's context call (`SinkAdapter::make`, `SourceAdapter::make`): a `std::logic_error`, or nothing made where it must make something | The adapter | The record, with the node and where (`configure <node>`); the node fails `lost` even if the type caught the throw |
| The same call's refusal (any other exception: no thread, no eventfd, no memory), and `std::bad_alloc` from NodeHost's own code there | Not a bug | The node's `start-failed`, `configure threw: <what>`, even if the type caught it; no record |
| A port block factory or an engine companion adapter, in the flush (each refuses through its events, never by a throw) | The adapter | The record, with the node and its flush item; the node fails `lost` (the block lost for good). Safe to run on because every adapter constructor is strongly exception-safe by construction (below) |
| An adapter's job on the I/O thread (a store's open or write: `StoreAdapter`'s calls, `where` the job, `the I/O job open store <file>`; a recording's open: `RecordingAdapter::open`'s `std::logic_error`, or no file made, `the I/O job open recording <path>`) | The adapter | The job catches its own throw and reports it through its configuration's completions: the record, with the node, and the node fails `lost`, as for any adapter bug (a recording adapter's other throws are its refusal: `start-failed: cannot open <path>: <what>`). Once the configuration ended, nothing is reported and what the job held is released on the I/O thread |
| A job's throw no adapter caught (the I/O thread's last-resort catch) | No owner but ear's core | The record without a node, `fatal` (`callback` `IoThread job`, `where` the job's name), told through the `InternalErrorListener` the thread was made with, then `std::terminate`; never a bare terminate |
| NodeHost itself, or any adapter call that interrupts NodeHost's own update (a block's `drive` or `carryLatency` as NodeHost carries out its actions), anything of core's an adapter's callback tells (a route link's listeners: the reconciler, and through it standard output and the gateway), the main loop's posted work and timers, the engine's signals, the live graph's registry, a sink's own thread | No owner but ear's core | The record without a node, `fatal`, then the line and `std::terminate` (above) |

Inside a context call NodeHost tells them apart by where they are thrown: around each adapter
call (`adapter()`), a `std::logic_error` is the adapter's bug and anything else its refusal;
around the whole call (`guarded()`), a `std::logic_error` of NodeHost's own checks is a rule the
type broke, `std::bad_alloc` is exhaustion (a refusal), and anything else of NodeHost's own code
is NodeHost's bug. A refusal passes as itself through every `guarded()` it is nested in, known by
the exception's identity until the type's call ends. ear's own fault travels
through the type's frames as a private `EarFault` naming its owner and is recorded where it was
made, so a type that catches it changes nothing; `callType` routes it: an adapter's becomes the
record and the node's `lost`, NodeHost's is rethrown as what was thrown, inside the hook's scope,
so the boundary names where.

An adapter whose fault the boundary catches tells its node through its events (`faulted` on
`PortBlockEvents` and `EngineCompanionEvents`), and the node framework's answer may itself throw:
what it throws is NodeHost's bug, reported again with no owner. A throw that came out of core
through an adapter's callback (NodeHost's inputs run inside the event that told them; a route link
tells the reconciler) is never the adapter's: only core's dispatchers leave a place with a throw
(a scope a throw ended, or NodeHost's rethrow putting one back, `MainLoopActivity::takeThrown`),
and an adapter that tells core anything but NodeHost does it inside a scope of its own (`Links`
tells its listeners inside `tell route links`), so the boundary takes a throw that carries a place
as core's, whichever callback it came out of, and does not tell the owner. A sink's
own code runs on its own thread, where no node can be failed in order with the rest, so
it has no owner and ends the process.

**Nothing stays registered.** Running on after an adapter's fault is safe only if what it
registered goes with it. Every registration the boundary makes is held by its owner as a handle
that removes it when destroyed: `addEvent`, `addTimer` and `addSignal` fill a `LoopSource`, each
`listen` a `Listener`, and a port block's entry in the levels registry is a `LevelsEntry`; proxies
and filters ear makes are held the same way (`OwnedProxy`). Each owner declares them in the order
it registers them, the object before the listeners on it, so a constructor that throws anywhere
unwinds every registration made so far before anything they call into goes, and its destructor
still removes each first, in its own order. Every step that would wait on the data loop (a blocking invoke into it,
removing a filter's listener, destroying a filter or a proxy) is taken in the boundary too, which skips it and leaks
once the loop is marked wedged ([below](#bounds-the-wedged-data-loop-and-the-watchdog)), so no path, an owner's fault
among them, can hang the main loop on it. A test seam counts what is held and throws after the
`n`th registration (`boundary::throwAfterRegistration`, `registrationsHeld`), and another throws
inside a named callback (`boundary::throwInCallback`), so the tests prove each owner both ways
([testing.md](testing.md#the-tools)).

## How a change moves

```
 editor / script                 hand edit
       |                             |
 control thread  --request-->  keeper thread  <--inotify--  ear.conf
                                     |  (candidate text, or the disk's text)
                                     v
                              main loop: keeper's main-loop side
                                     |  DesiredGraph::plan  -> refused: reply only
                                     |  accepted: tell the file change, commit, reply
                                     v
                   DesiredGraph observers (gateway)  then subscribers
                                     |                       |
                                 NodeHost               Reconciler
                                     |                       |
                          node health, notes        route links, route status
                                     \                       /
                                StatusLog (stdout) and gateway (clients)
```

1. A request reaches the keeper's thread whole (a batch stays whole). For an edit the keeper
   loads the working copy's text afresh and applies the edit at its source, making a
   *candidate* ([graph-file.md](graph-file.md#edits-at-the-source)). An edit of a node's settings
   that strands a route on a port it takes away is refused there, or with `dropRoutes` takes the
   route lines out of the same candidate ([control-port.md](control-port.md#changes-of-ports)).
2. The keeper posts the candidate to the main loop, where its main-loop side asks
   `DesiredGraph::plan` for a verdict against the whole wanted graph.
3. If accepted, in one main-loop turn: the file change is told (gateway, then `StatusLog`), the
   plan is committed, its node and route events follow, and the reply is sent.
4. The keeper keeps the candidate as its working copy and writes `ear.conf` once no step is
   waiting (write-behind): it swaps the new text in and keeps it only if the text it replaced is
   what it last read or wrote.
5. A refused candidate is dropped; nothing is told or written, and only the reply carries the
   refusal. A dry run's candidate (`node.set` with `dryRun`) is planned and answered, then dropped,
   accepted or not: nothing is committed, told, recorded or written
   ([control-port.md](control-port.md#nodeset-and-dry-runs)).

**A type action** (`node.invoke`, [control-port.md](control-port.md#actions)) rides the keeper's
order as a lifecycle verb does: it needs no file, and its step hands it to `NodeHost` on the main
loop (`MainLoopSide::invoke`, `NodeActions::invoke`) and ends there. The keeper never waits for a
handler. The batch's reply is held on the main loop (the keeper's `Waiting`) until every action in it
answered, at once for a trigger or a refusal, later for a call, at most 10 s
([rates](#rates-and-bounds)), so it goes behind every event before it.

A hand edit arrives as an inotify event, is read on the keeper's thread and offered the same way
(cause `disk`). The graph may refuse it, but the working copy still follows the disk, and the
graph keeps running what it ran.

**Order.** `DesiredGraph` tells its observers (the gateway) of each change before its subscribers
(`NodeHost`, then the `Reconciler`), and a commit's events always come in one order: routes
removed, nodes removed, nodes changed, nodes added, routes added. `LiveGraph` tells the
`Reconciler` which ports exist; `Links` tells it when a link is active, failed or gone. `NodeHost`
and the `Reconciler` tell `StatusLog` and the gateway. So a change's notification always comes
before the statuses it causes, on standard output and on the wire alike.

**Reconciling routes.** A route is linked whenever both its ends are live ports, whatever either
node's state: the `Reconciler` asks `Links` for a link by port ids; the route is `linked` once
the link is active, `pending` while an end is missing, and `refused` with a detail when the
engine refused the link. A wire never depends on a node's state.

## How audio moves

1. **The engine.** `Engine` runs PipeWire's engine in-process with ear's internal clock driving
   the graph at the graph frequency, one block of `block_size` frames a cycle, both fixed at boot
   ([engine.md](engine.md)).
2. **Port blocks.** Each node has one port block (a `pw_filter`) for its whole life, named by the
   node's name, with one 32-bit float mono port per declared port, public and inside alike. Every
   block is in the graph group and always processes.
3. **A configuration.** The node's type makes an instance whose `configure` acquires resources
   (sound cards, RTP and pipe receivers, recordings, sinks, sources, a program) and gives the code that will run. Once every
   resource is ready and the block is ready, the framework drives the block with that code
   ([the node lifecycle](#the-node-lifecycle)).
4. **Each cycle**, on the realtime thread, the block calls the code with one span per port and a
   `GraphCycle` (the cycle's start on both monotonic clocks, the driver's position, rate and id,
   the next cycle's start, an xrun count, whether the clock jumped, and the latency downstream of
   each output), so no type reads a clock ([engine.md](engine.md#graphcycle)).
5. **Routes.** `LiveGraph` reports ports as the engine's registry announces them; the
   `Reconciler` links wanted routes between live ports. An unlinked input reads zeros.
6. **Engine companions.** A `card`, `rtp` or `airplay` node's block has inside banks no route can name; the
   framework joins an ALSA adapter node, an RTP receiver or a pipe receiver to them, and its pass-through copies
   between inside and public banks ([engine.md](engine.md#engine-companions)).
7. **Off the realtime thread.** A `file` node's block writes each cycle into a sink's lock-free
   ring, read by the sink's own thread ([engine.md](engine.md#sinks)); a source's ring, written by
   an ordinary thread, is played by its block before the code ([engine.md](engine.md#sources)).
   Both are [companions](#companions), as cards and RTP receivers are.
8. **Levels.** Each block meters its public ports and publishes a window every 1/30 s of graph
   time; the control thread sends the newest to subscribers
   ([control-port.md](control-port.md#levels)).

## Status and events

A node reports what it observes as **status** (fields with a current value) and **events**
(things that happened), declared in its spec
([writing-a-node-type.md](writing-a-node-type.md#status-and-events)) and written off the realtime
thread through `Report<spec>` ([writing-a-node-type.md](writing-a-node-type.md#report)). *Why:* the
owner's rule, "Nodes should not have direct access to stdout or stderr. The core framework should
expose the necessary primitives for events, diagnostics, and logging", and status is "view +
change record": a live view on the wire, coalesced, and every change on standard output, paced so
each field's last record is its value.

- **One queue per configuration.** `Report`'s writes and the handle's `lost` go into the
  configuration's queue, on its channel ([teardown](#teardown-by-dependency)), under one short
  mutex: bounded at 1024 records, a full queue drops a status write or an event and counts it
  (`ear.records-dropped`, subject `queue`), never a loss. A text is made safe there, on the
  writer's thread. One writer's records stay in its order, so a status write before an event is
  drained before it, and a `lost` after the last status after it.
- **The wake.** The first record into an empty queue posts one wake to the main loop, which marks
  the node dirty and arms the **tick** on the main loop's timers: 33 ms after the last tick, or at
  once if that has passed, so at most 30 ticks a second for all nodes together, and none while
  nothing is written. A `lost` is never paced: it posts a drain of its own, which drains the queue
  up to it at once.
- **The drain**, at each tick, node by node in arrival order: a status write updates the node's
  **status view** (a write of the value a field holds is no change), is offered to standard
  output's per-field pace and marked for the wire; an event first sends the node's marked fields,
  then is offered to the node's event pace; a `lost` first sends the marked fields, then fails the
  node as built (dropped once its configuration's teardown began). At the tick's end, one
  `statusChanged` per node with the fields marked since the last.
- **What the view belongs to.** The view is the node's, held by NodeHost: every field at its
  initial value until written. It belongs to the configuration: when a configuration closes,
  fails to start or is leaked, its last records are drained, nothing more of it lands (its channel
  is detached), and the view returns to every initial value, told as one reset after those last
  values. On a restart that falls between the old configuration's `stopping` and the new one's
  `configuring`. A stop, a start and a change in place keep the configuration, so they keep the
  view and send no reset. A write lands until the configuration closed, so a goodbye written
  during its teardown is heard.

NodeHost tells its `NodeStatusListener`s each change as standard output writes it
(`statusWritten`), the wire's coalesced changes (`statusChanged`), each reset (`statusReset`) and
each event (`eventRaised`); a superseded node's only to listeners that hear one. Core reads no wall
clock: each is stamped on the main loop's steady clock, and each listener maps it to UTC.

### Rates and bounds

| What | Bound | Policy |
|---|---|---|
| The tick | 30 a second, all nodes together | armed only when something is written or due |
| `statusChanged` | one per node per tick | the latest values |
| A node's queue | 1024 records | a status write or an event dropped and counted (`ear.records-dropped`, subject `queue`); a `lost` never |
| Standard output's status records | per node and field: 32 at once, then one a second | once the pace holds a change back, every later change of the field is held too, until a **trailing** record writes the view's value with `heldBack`, the count, at the first tick the pace allows (at most 1 s after the last held change); so the last record of each field always matches the view. The held changes are also counted in `ear.records-dropped`, subject `status` |
| Events (wire and standard output alike) | per node: 32 at once, then one a second | held back ones counted in `ear.events-dropped` (`heldBack`) |
| `ear.events-dropped`, `ear.records-dropped` | at most one a second per node (and subject) | the count since the last |
| Flapping | a field with more than its `flapsAbove` changes (default 10 in 10 s) within the window | `ear.status-flapping`, subject the field, active (`transitions`, `windowMs`) when it crosses, said again at most once a window while it stays above; inactive once a whole window passed with it at or below |
| A call's answer | 10 s (`actionDeadline`), timed by NodeHost on the main loop's timers | answered `timeout`; the handler's late answer dropped ([writing-a-node-type.md](writing-a-node-type.md#actions)) |

A node diagnostic of these is the node's, kept per code and subject as the framework's own
([output-records.md](output-records.md#diagnostic)): `ear.status-flapping` is a condition,
`ear.events-dropped` and `ear.records-dropped` observations.

### The realtime channels

A node's code reports from `process` through `Io`
([writing-a-node-type.md](writing-a-node-type.md#reporting-from-the-code)), into channels its
configuration owns (`ear/core/realtime_channels.hpp`). *Why:* the owner's rule that the
framework's primitives for events, diagnostics and logging "should be real-time safe for any audio
loops that need them", and that the audio threads only read inputs and write outputs, so the code
never touches a queue, a lock or a clock of its own.

| Channel | Mechanism | Bound | On overflow |
|---|---|---|---|
| An `rt` status field | one `std::atomic<std::uint64_t>` per field (a flag 0 or 1, a number or decimal its bits, a choice its index) and an `std::atomic<std::uint32_t>` transition counter beside it, both stored by the code, sampled by the drain | 64 `rt` fields per type | none: the word always holds the last value |
| The ring | single producer (the code), single consumer (the drain), fixed 48-byte records: kind, index, count, the cycle's `startNs`, up to four 8-byte values | 256 records per configuration; a log record is refused once 192 are in use, so status and events keep the last quarter | dropped, counted per kind: `ear.records-dropped`, subject `realtime` (`count`, `status`, `events`, `logs`) |
| The bell | `RealtimeBell::ring()` (ear_pipewire's: [engine.md](engine.md#the-realtime-bell)), gated by one atomic exchange per configuration: rung after a `process` that wrote anything, unless it was rung since the last drain | at most once per drain, and the drain is the tick's, so at most 30 a second per node | — |
| A broken value | the first value outside its declaration, kept once | one | the node fails `lost` with the rule |
| A trigger (in) | one `std::atomic<std::uint32_t>` counter per trigger, moved by the main loop; beside it the last value the code saw, the code's alone ([writing-a-node-type.md](writing-a-node-type.md#actions)) | — | several invokes before a block are one |
| A live slot (in) | one `std::atomic<std::uint64_t>` per live member (a decimal's bits, a whole number, a boolean 0 or 1, a choice's index), written by the main loop ([writing-a-node-type.md](writing-a-node-type.md#live-rows)) | 256 live members per type, each family at its count row's maximum | none: the word holds the latest |
| The generation word (in) | one `std::atomic<std::uint64_t>`: the write count in its low 32 bits, moved after each batch of slot writes, and the snap epoch in its high 32, moved before each drive; stored with release by the main loop, its one writer, which keeps its own copy, so neither side needs a read-modify-write | one | — |
| Ramp and step state | per member, the code's alone: a ramp's ends, first frame and last unsteady frame, a step's value, its value before its last edge and where that edge was | as the slots | — |

`io.set` stores the word and, when the value differs from the last one the code wrote (its
initial value before any), counts a transition and puts a status record in the ring. `io.raise` and
`io.log` put one record each. The code's side is all stores, one fence and one exchange: no allocation, lock,
format or system call but the bell's (`ear_core_tests` counts the allocations of a code writing every
kind, none). The bell's gate is cleared by the drain before it reads the ring, and a sequentially
consistent fence on each side (after the clear; after the cycle's writes, before the exchange) makes
the two meet: either the drain reads the cycle's writes or the exchange sees the clear and rings.
Without them a store buffer can hold the clear while the drain reads the old head, and the write is
neither drained nor rung for until the code writes again.

**The advance.** Before each `process`, the code's side of the live slots advances
(`RealtimeChannels::advanceLive`): the block's position moves on by its frames, and the generation
word is loaded (acquire). When it is the one last seen, that is all: one load and no
read-modify-write, whether or not a ramp is running, since a view is computed from the ramp's ends
and where the block sits in it. When its snap epoch moved, every member is seated at its slot with
no ramp and every step unchanged. Otherwise every slot is loaded (relaxed, after the acquire): a
ramped member whose slot moved starts a ramp from the value its last frame had, a stepped one has
an edge. Positions count the frames this configuration's codes processed, so a ramp and a step's
edge are where they are whichever code runs. It never allocates, locks, formats or makes a system
call (`ear_core_tests` counts the allocations of 130 members moving, none). A cycle that does not
run the code ([engine.md](engine.md#the-quantum-guard)) does not advance it: the block writes zeros
anyway. Members written in one batch start their ramps in one block, except when the load falls
inside the batch: then the rest start one block later. A seqlock would close that gap with a retry
loop on the realtime thread, for at most one block of ramp.

**The snap.** `NodeHost` moves the snap epoch before each drive of the code (its first, and a
start after a stop), never before a swap, since the block cannot tell a drive from a swap
(`CycleCause::drive` is set for both: [engine.md](engine.md#drive-and-fade)). So slots moved while
the node was stopped or not yet driven are taken at once under the drive's own fade-in
([fades](#fades)), and a code a change swaps in goes on with every ramp and step where the old one
left it.

#### Live values

Each node keeps its **live values** in `NodeHost` (main loop): every live member's value as its slot
holds it, one per member up to each family's maximum. They are seeded from the node's settings when
it is added (`NodeConfig::liveValues`), seed every configuration's slots when it is made (never
from that configuration's config, whose own may be older), and are what `s.liveValue` reads in
`configure` and the change version (bound for the call: `CreateContext::liveValues`). They belong
to the node, not to a config: `NodeHost::setLive` moves them and the configuration's slots at once,
in any phase (with no configuration, only the live values), and never adopts or replaces a config.
A [retune](#retunes) is what moves them from the graph file; a change's commit point writes no slot,
since a change's keys are never live.

**The drain**, on the main loop, is part of the tick's: the bell's dispatch arms the tick as a
queue's wake does (`NodeHost::realtimeRung`), and each node's drain reads its configuration's ring
first, then its queue, then samples each `rt` word the code wrote since the last sample. A ring
status record is a status write as `Report`'s is; a sampled word whose value differs from the view
(its record had no room) is one too; an event is an event; a log record is written to the log's
sinks there, a `log` record of the node with the code as its `topic`. A realtime record is paced at
the drain's time, but carries its cycle's start: an event's `t` is when its cycle began. A
field's transitions for flapping are its counter's moves between samples, so changes whose records
had no room still count. A broken value fails the node after everything else drained.

**Across the lifecycle.** The channels are made with the configuration (each `rt` word at its
field's initial value) and freed at teardown step 5, after the code; nothing is allocated, freed or
resized on the realtime thread or while a code that uses them can run. The code is bound to them
when `configure` answers (`CreateOf::applied`), and a code a change swaps in is bound to the same
ones (`ChangeOf::applied`), so each `rt` field keeps the value the old code last wrote. A stop and a
start keep them; a restart, a failure or a removal drains them with the queue as the
configuration closes, and its successor starts from new ones. One node's channels are never
another's. A live slot is kept across a stop, a start, a change in place, a reshape in place and a
swap; each configuration's is seeded from the node's live values. A type whose code writes nothing
on the realtime thread and reads no trigger or live row (no `rt` field, event, log code, trigger or
live row) has none.

## Companions

A type's realtime code reads its input banks and writes its output banks, and nothing else. *Why:*
the owner's rule, "The node audio threads should be concerned with reading inputs and writing
outputs. The framework should provide the necessary primitives to connect inputs/outputs across
nodes AND anytime a node needs to cross inputs/outputs to non-realtime code". So when audio must
leave the realtime thread (to a file, a network library, a device) or come onto it from an
ordinary thread (a decoder, a library's thread), the type declares an inside bank and asks in
`configure` for a **companion** on it, and its code passes audio between its public and inside
banks: it never touches a ring, a wake or a device. The shape is the owner's too: "crossing to a
non-rat [non-realtime] thread could be managed by allowing a node to internally pipe its
inputs/outputs to a hidden node, to maintain the integrity and consistency of the processing
graph".

There are two families, and the kinds are a closed list (`CompanionKind`,
`ear/core/companions.hpp`), each with one adapter in `ear_pipewire`. *Why closed:* a kind changes
what the framework promises (when it is ready, what it adds to latency, what fails its node), so a
new one is a change of the framework, never a type's own.

| Family | Kinds | What it is | How it works |
|---|---|---|---|
| Engine companion | a sound card's playback, its capture; an RTP receiver; a pipe receiver | A PipeWire node of its own, joined channel for channel to the bank's ports by links | [engine.md](engine.md#engine-companions) |
| Ring companion | a sink, a source | A lock-free ring between the block and an ordinary thread, served by the block itself in its own cycle: it plays a source into its bank before the code and taps a sink's bank after it | [engine.md](engine.md#sinks), [engine.md](engine.md#sources) |

- **Hidden, not secret.** A companion is never in the desired graph, never named by a route and
  never metered (only public ports are); the control port lists it under its node
  ([control-port.md](control-port.md#companions)) and the editor draws it as a badge in the node's
  box ([editor.md](editor.md#companions)). An engine companion is named by its kind
  (`<node>.playback`, `<node>.capture`, `<node>.rtp`, `<node>.pipe`), a ring companion by its bank
  (`<node>.<bank>`); a node's name has no `.`, so a companion's never equals a node's.
- **One per inside bank,** of any kind. *Why:* the block sizes its storage for bindings when it is
  made (one source per inside input bank, one sink per inside output bank), so nothing allocates on
  the data loop.
- **Its failure is its node's alone** (the owner's per-node rule: "Some node
  stopping/reconfiguring/failing does not effect the graph globally"). An engine companion lost or
  not ready by its deadline fails its node `engine-refused`; a sink whose consumer does not begin
  in time fails it `start-failed`, since `begin` is the type's code
  ([engine.md](engine.md#readiness-deadlines)). The node's block and wires stay.

**Why a sink or a source is served by its block, not an engine node of its own.** Asked whether a
sink or a source should be its own hidden PipeWire node or a helper its node's block serves in its
own cycle, the owner answered "Served by the node". Cards and RTP and pipe receivers stay engine nodes:
the device, the module's socket and the module's FIFO are PipeWire's. The reasons:

- **It is exact by construction.** A ring companion is bound and unbound by the drive's own invoke
  with the code ([engine.md](engine.md#drive-and-fade)), so the source, the code and the sinks run
  in one `process`, at one position, and change together between two cycles; a cycle the block
  does not run, none of them runs. As nodes of their own they could disagree: an invoke can land
  between a block and another node within one cycle (inside one `process` nothing can), and a
  cycle an xrun cuts short is run by an upstream prefix of the graph only
  ([engine.md](engine.md#the-one-data-loop-and-invokes)), so a source upstream of its block could
  play a cycle the block never ran. Nothing is armed by position or deferred to a cycle, so the
  wedge is still judged in one stage: a barrier still pending means the loop ran no invoke
  ([bounds](#bounds-the-wedged-data-loop-and-the-watchdog)).
- **It is cheap.** Serving one costs a copy a cycle (about 1 µs for stereo at 48 kHz and 256
  frames): no engine node, eventfd wake or graph recalculation per crossing, so a `file` node is
  one engine node.
- **Nobody outside ear's code sees the difference.** ear runs PipeWire in-process with no outside
  tools; the type's call (`cx.attach`), the wire's entries and the editor's badges
  are the same either way, and the owner's aim holds: the type's code reads and writes only its
  banks, and the crossing is the framework's.

## The timing model

ear's audio runs on one tick at one rate, and every other clock belongs to the node at the
graph's edge that brings it in. *Why:* the owner's model, after `sendspin` showed that time the
framework keeps for a node is kept again by whatever the node talks to, and the two correct the
same lost time. It reversed the owner's earlier answer (2026-11-10) that what a source reports as
played keep in step with the clock across a stall longer than its buffer: the framework's played
count that kept it is deleted, and a source with a clock of its own keeps its time itself.

1. **The tick.** The framework supplies one tick, the graph cycle, and states its facts at the
   graph rate: its start on the raw clock, whether it began overdue, and what was lost or cut
   before it and why (`GraphCycle`: [engine.md](engine.md#graphcycle),
   [writing-a-node-type.md](writing-a-node-type.md#lost-cycles)). It keeps no audio time for any
   node.
2. **Node kinds.** Every node is a source, a sink or a transform. A transform reads its inputs,
   processes and writes its outputs purely on the tick: it keeps no clock, skips nothing and dates
   nothing (the pass-through between a node's banks is one). A node at the edge in both directions
   (a `card`, a `sendspin` node) is a source on its capture side and a sink on its playback side.
3. **Clocks belong to sources and sinks.** A source or sink with a clock of its own (a server, a
   network sender, a sound card) reconciles that clock with the tick inside itself: buffering,
   rate matching, drift, and recovery after lost ticks. A card's ALSA node and an RTP receiver do
   it in PipeWire ([engine.md](engine.md#sound-cards), [engine.md](engine.md#rtp-receivers));
   a type's source does it in the `SourceClock` it gives the source, which alone decides what the
   ring skips ([writing-a-node-type.md](writing-a-node-type.md#a-sources-clock)): `sendspin`'s
   `SendspinClock` ([node-types.md](node-types.md#clock)). A node's own-clock side may keep time
   without the tick through a stop or a host stall: past its bound with no tick, `sendspin`'s clock
   plays what its library writes into nothing at the graph rate and tells it so, on the same one
   timeline the ticks advance, so the library is never held back by a graph that does not tick (the
   rule's home: [node-types.md](node-types.md#clock)).
4. **One rate inside the graph.** Every port in the graph runs at the graph rate, and conversion
   happens only in edge nodes. The rate is configurable (`graph { rate 48khz; }` in `ear.conf`, the
   `--rate` flag as fallback: [boot](#boot)) but fixed for the engine's life, and it is
   the only rate allowed ([engine.md](engine.md#the-clock)). A graph of mixed rates, two graphs
   joined by a converting edge, is out of scope.
5. **Conversion is a converter companion.** Rate and drift conversion is PipeWire's converter
   (audioconvert in an adapter, with its resampler) between the node's own side and the graph,
   hidden from the graph file and owned by the node; the node either sets its ratio or lets it
   track a rate the node reports. A card's ALSA adapter and an RTP receiver's stream are such
   converters inside their engine companions ([engine.md](engine.md#engine-companions)); the RTP
   receiver is the first converter companion, in module-run mode, where a PipeWire module owns the
   stream, its resampler and its loop ([engine.md](engine.md#module-run-converters)). Simple work in
   code, such as converting a sample format, stays a library function.
6. **The internal clock drives.** Cards follow it and adapt to the graph, never the reverse
   ([engine.md](engine.md#the-internal-clock)).

**Where the code still departs**, removed by
[`design/slice-3c-remainder.md`](design/slice-3c-remainder.md):

- The converter companion has only its module-run mode
  ([engine.md](engine.md#module-run-converters)), whose users are the RTP receiver and the pipe
  receiver: there is no set or tracked converter yet for a ring whose clock is a type's own, so only
  cards and those receivers convert.

## The node lifecycle

A node is two things: its **block**, made once and kept for the node's life, and its
**configuration**, made from its settings and torn down and remade behind the same block. The
framework drives both through two pure state machines (`node_lifecycle.hpp`), and `NodeHost`
carries out what they decide.

### Phases, states and what is wanted

A node's **phase** is its exact state; its **state** is the phase's colour, which the editor and
standard output show.

| Phase | State | What it means |
|---|---|---|
| `added` | `starting` | The node is in the graph and its block is being made. It waits here while an older node of its name (a removed or renamed one) is still closing |
| `configuring` | `starting` | Its instance was made and `configure` ran; the resources it asked for are being readied |
| `configured` | `starting` | Every resource is ready; the code is prepared |
| `starting` | `starting` | Wanted running, waiting for the block to be ready and the code to drive it |
| `running` | `running` | The block runs the node's code |
| `stopping` | `stopping` | Fading out, then silencing; the `cause` says why: `stop`, `disabling`, `removing` or `restarting` |
| `stopped` | `stopped` | Silent, keeping its configuration |
| `disabled` | `disabled` | Silent, with no configuration |
| `failed` | `failed` | Torn down after a failure; `reason` says why |
| `reconfiguring` | `reconfiguring` | Applying new settings in place, from `running` or `stopped`, without a restart ([changes in place](#changes-in-place)) |

`added`, `configured` and `starting` normally last milliseconds. **What the graph wants** of a
node (`wanted`) is `disabled` when its block in `ear.conf` says `disabled true;`, else `stopped`
while a stop is held, else `running`. A node that rests stopped shows `stopped` only once it is
there; until then it shows `starting`.

### Stopped versus disabled

| | Stopped | Disabled |
|---|---|---|
| Set by | `node.stop`; cleared by `node.start` | `node.disable` or `disabled true;` in the node's braces; cleared by `node.enable` or a hand edit |
| Survives a restart of ear | No: never written to `ear.conf`, forgotten at restart (the node then runs) | Yes: it is in `ear.conf` |
| Configuration | Kept: instance, resources, a card's open device, a `sendspin` node's connection | Released: nothing is acquired, a card's device is closed |
| Block, ports, routes | Kept | Kept (its input meters still show what reaches it) |
| Coming back | `node.start` drives the code again within a turn and a couple of cycles; it cannot fail for a missing device | `node.enable` configures it from empty, which can fail like any start |

A node that boots disabled is never configured. `node.start`, `node.stop` and `node.restart`
refuse a disabled node (`node-disabled`): enable it first.

### Restarts and changes of settings

`node.restart` tears the configuration down and makes a new one behind the same block, returning
to the wanted state (a stopped node is configured again and rests stopped). Every change of a
node's settings does the same (cause `restarting`, then `configuring` with the changed keys).
An update that leaves the settings as they are changes nothing, for a failed node too. A change
of a node's ports (a card's channel count) keeps the node and its block, reshaped
([below](#reshapes)); a change of its type replaces the node: its routes and the node are removed
and added again. A change to a disabled node is stored for when it is enabled.

The desired graph's plan says what each edit asks of a node it keeps by name (`NodeOutcome`, told
through `nodeChanged` with a `NodeChange`). It first splits the changed keys by their rows' marks
([writing-a-node-type.md](writing-a-node-type.md#change-marks)): the **live** keys go to
`NodeChange::retuned` and are [retuned](#retunes) beside whatever outcome the others decide; the
outcome is then `disable`, `enable` or `stored` by its `disabled` before and after; otherwise
`reshape` when its ports differ, `unchanged` when no key does (nothing is done or told), `retune`
when only live keys do, `reconfigure` when every other changed key's row is marked in place, else
`restart` (every other row is marked to restart, with the reason the type schema carries). A family
member's key (`trimDb.2`) is judged by its family's mark (`NodeType::changeOf(key)`, through the
table's one key lookup: [writing-a-node-type.md](writing-a-node-type.md#indexed-families)). A
`reconfigure` is taken in place when the type has a change version ([below](#changes-in-place)),
else as a restart. `NodeChange::keys` holds only the keys that are not live, so a live key never
appears in a health's `keys` or `next` or a restart's cause. `changeOf` (`desired_graph.hpp`) is
the one rule, the plan's and the type harness's.

#### Retunes

A change of live keys moves values only the code reads, so it is taken with no hook called, no
reconfiguring and no health told: `NodeHost` writes the node's [live values](#live-values) and then
its configuration's slots at once, in the turn the plan is committed and in any phase, before it
takes the outcome the other keys decide. The values come from the change's new config
(`NodeConfig::liveValues`), member by member for the keys retuned, so a member another key's move
set is kept. A retune never adopts or replaces a config: the configuration keeps the config it was
made or changed with, and the newest is kept for the next configure.

| Phase | The retuned keys | The outcome (the other keys) |
|---|---|---|
| `running`, `stopped` | live values and slots move | as built |
| `reconfiguring`, before the commit point | live values and slots move; the change and its target are untouched | a `retune` never supersedes the change in flight; any other outcome as built |
| `reconfiguring`, committing | live values and slots move | as built: waits for the invoke |
| not yet rested, `failed`, disabled | live values move, and the slots of a configuration it has; they seed the next | a `retune` restarts nothing, skips no wait and retries no failure; a disabled node's change of live keys alone is `stored` and starts nothing |
| a restart in flight | live values move, and the old configuration's slots, heard through its fade-out; the new one is seeded from them | as built |

So a fallback, a supersession or a stop of a change cannot revert a retune: each configuration is
seeded from the live values, and the commit point writes no slot. A live row can never be why a
node failed (it touches nothing but the code's view), so a change of live keys alone does not
retry a failed node, and a fader's drag cannot start one. A loop's state and companions are not
told of a retune. Its records are standard output's `param` records
([output-records.md](output-records.md#param)); on the wire it is `applied: "retune"` with
`retuned` ([control-port.md](control-port.md#retunes)).

**Checking a retune.** A type's `check` reads live rows through `liveValue`. At parse it judges
the parsed values, which a change's new settings hold whole. `NodeConfig::checkLive(live)` runs it
with given live values in place, the type harness's `live` calls it before it moves a slot, and a
refusal moves nothing.

### Changes in place

A change in place applies new settings to a node's configuration without tearing it down: a
change the plan says is a `reconfigure`, on a node whose type has a change version of
`configure` ([writing-a-node-type.md](writing-a-node-type.md#the-change-version)); a type with
none takes it as a restart. The node shows `reconfiguring`, with `keys` (the keys the change
applies) and `next` (keys changed since its commit point, which it takes once the change ends).

What NodeHost does, in order:

1. **Calls the change version** on the main loop, timed as `configure`, with `old` the applied
   settings and `now` the change's target; the configuration still applies the old ones. Its
   context offers holds, replacements, claims and unclaims, staged posts, a latency declaration
   and the answer. Its claims are judged as it returns, before anything else
   ([writing-a-node-type.md](writing-a-node-type.md#claims-in-a-change)).
2. **Holds** the banks it names: told to the block only while the node is driven (a stopped node
   is silent already). The commit point waits for each hold's acknowledgement, at most 100 ms
   (past it `ear.fade-unacknowledged`, and the change goes on).
3. **Answers.** A new code is prepared now, while the old one runs (when the old one is
   prepared), and its latency plan worked out; a broken rule, a pass-through or a plan that does
   not fit fails the node `lost`. An answer `restart`, or a change version that threw, falls back
   (below).
4. **The continuation**, when the change version answered `whenReady`
   ([writing-a-node-type.md](writing-a-node-type.md#whenready-and-configuredone)): once every
   acquisition is ready and every hold acknowledged it is posted to its loop, and the commit point
   waits for its `done()`; a `fail(detail)`, or its token dropped, falls back (`failed`).
5. **The commit point.** The instance adopts the new settings and the configuration holds them;
   the staged posts are released to their loops, in the order posted; the plan is carried; a new
   code, or a replaced sink's or source's binding, is swapped in one invoke, its completion the
   commit's (from `prepared`, or with neither, the commit completes in its own turn); the old code is released once its swap completed;
   then the holds fade back in; last, the ports the configuration no longer asks are released.
   The loops and their states go on: nothing is torn down.
6. **The deadline**, 10 s from the change version's call (a type declares 1 s to 60 s: its spec's
   `.lifecycle`): past it the change falls back, reason `deadline`.

The hold's fade (5 ms), its acknowledgement's bound (100 ms) and the deadline (10 s, at most 60 s)
are first choices, not yet tuned on real hardware (the MOTU).

A **replacement** (an RTP receiver or a sound card on the same bank, a sink or a source on it, or
a recording: [writing-a-node-type.md](writing-a-node-type.md#make-before-break),
[#sinks-and-sources-in-a-change](writing-a-node-type.md#sinks-and-sources-in-a-change),
[#recordings-in-a-change](writing-a-node-type.md#recordings-in-a-change)) is readied beside the old
one, in readiness order, once the change version answered: the change's new claims first, its
recordings' among them (a refusal falls back, `resource-refused`, before anything breaks), then
its recordings opened, then its companions: each replacement sink's consumer begun, and, behind a
hold the framework takes on what its bank carries, each engine companion made beside the old one
(an RTP receiver on a new port, a sound card), or break-first, the old one destroyed and the new
one made in one flush (an RTP receiver on the same port, a card the type orders so). The commit
point waits for every one to be ready (an engine companion's loss first falls back,
`resource-lost`); then it destroys an old engine companion that still runs, moves each sink's and
source's bank to its replacement in its invoke (the old sink finishing once that completed, the
old source's writer closed), closes a replaced recording once every replaced sink finished, and
releases the ports the old ones held that the new ones do not, last. An old one is retiring
meanwhile (listed `retiring`): an engine companion's loss is `ear.retiring-lost`.

A change that ends without committing (superseded, stopped, failed or fallen back) destroys its
staged posts unrun and releases its new code and the ports it was granted, closes its
replacement sinks, sources and recordings (a replacement made
break-first stays the bank's companion, its ports the configuration's; one broken but not yet
made, its flush still to run, is made only if no newer change broke the same bank first: the
newest break's replacement is the one made; one made before break is destroyed, and the one it
was made beside is the bank's again);
its holds stay until the next commit or the silence. A supersession says `ear.reconfigure-superseded`, a fallback
`ear.reconfigure-fell-back` before its `stopping` (all but `restart-requested`, which the operator's own
`node.restart` asked for; its `stopping` record names the reason) ([output-records.md](output-records.md#diagnostic)).

The configuration machine has two states for it. **`changing`** begins from `driving`, or from
`prepared` once the configuration has rested (a change before it first rested is not taken):
the change version is called (`reconfigure`) while the configuration still applies the old
settings, and it lasts until the change is ready to commit. A newer change in place in
`changing` ends the change uncommitted (`dropChange`) and calls the change version again. A
stop, a teardown or the block's loss ends it uncommitted too, then does what the state it began
from does (a stop of a change from `prepared` has nothing driven to stop, so the change goes
on). The **commit point** (`commit`) from `prepared` needs no invoke and returns there at once;
from `driving` it is one barrier-numbered invoke on the data loop, **`committing`**, which
cannot be recalled: a stop, a teardown or a newer change waits for its completion, which returns
to `driving`. Only the block's loss (a silence, as from `driving`) and the invoke's 5 s bound
(the data loop wedged, the configuration leaked, as for a drive) move the configuration before
it completes.

The node aggregate decides when a change of settings is taken in place, and what arrives during
one:

- **Who takes it.** A change the plan says can be made in place is taken in place only by a node
  that has rested (reached `running` or `stopped` once), with no stop, restart, disable, removal
  or failure in flight and the data loop not wedged; from `running` or `stopped` at once, and
  after a start's drive or a stop in flight once it ends (a change that waited for a start's drive
  begins in the drive's own batch, before `start()` runs; a `start()` that throws there still
  fails the node `start-failed` with no `stop()`, ending the change). Any other node takes it as a
  restart-only change: one that has not yet rested restarts (nothing has started, so nothing is
  gained), a failed one retries, a disabled one stores it, all with cause `settings` as above.
  Changes that wait together are made in place only if every one of them could be.
- **The applied settings** become the change's only at the commit point, so a change that ends
  any other way leaves its keys to apply.
- **Before the commit point** a newer change in place supersedes the one in flight: the change
  version is called again with the newest settings and every key not yet applied. A newer
  restart-only change restarts the node (cause `settings`, naming those keys). A stop ends the
  change and stops; once stopped, the change is made in place from there. A disable or a
  removal ends the change and goes on as from `running` or `stopped`. `node.restart` falls back
  at once.
- **After the continuation's post** a newer change waits for the change's end, then is made
  afresh (in place, or a restart if any key of it must restart); a stop, a disable, a removal,
  `node.restart`, a fault and the deadline still end the change, the continuation running on its
  loop all the same, its `done` ignored.
- **After the commit point** (the commit's invoke in flight) a newer change, a stop, a disable, a
  removal, a fault's teardown or `node.restart` waits for the invoke, then applies: a newer change
  is a fresh change (in place, or a restart if any key of it must restart), a restart falls back.
  A fault shows `failed` at once.
- **The fallback.** A change that cannot finish restarts the node with the newest settings, never
  the old ones, which `ear.conf` no longer holds: `stopping` with cause `restarting`, its own
  kind `fell-back`, the change's keys, a `reason` and a `detail`, then `configuring` with that
  cause. The reasons: `restart-asked` (the change version answered restart), `deadline`,
  `resource-refused` and `resource-lost` (a replacement refused, or lost before the commit
  point), `failed` (the change version failed), `restart-requested` (`node.restart`).
- **A fault** during a change (a kept resource lost, the block lost, a hook that threw) fails the
  node as in any other phase; it is not a fallback.

### Reshapes

A change of a node's ports with its type kept (a card's `playback` count, an `rtp` node's
`channels`) is a **reshape**: the node keeps its block, and the block's ports change to the new
ones, the links on the ports both name kept throughout ([engine.md](engine.md#reshapes) says how
the block does it). The plan's `NodeChange` carries the ports before and after.

- **Routes.** Every wanted route must name a port the new ports declare, as for any edit (a route
  to `desk:play.4` while `desk` goes to two channels is refused `unknown-port`), so no wanted route
  names a vanishing port by the time a reshape commits, and a route kept is never removed or added:
  routes are removed before nodes change in a commit. A route may name a port that appears; it links
  once the port is live. Through the control port, the graph file keeper refuses such an edit first
  as `ports-in-use`, naming every route, or with `dropRoutes` removes those route lines in the same
  edit ([control-port.md](control-port.md#changes-of-ports)).
- **As a restart** (the default): the old configuration is torn down as for a restart, then, at the
  new configuration's start, the block, silent, takes the new ports, and `configure` sees them.
  Surviving ports keep their links and carry zeros while the node restarts; inside banks are resized
  with the rest, and their companions are made afresh by the new `configure`. The cause is
  `reshape`: the changed keys and the public ports added and removed against the ending
  configuration's (a later change that brings the ports back before the restart is `settings`
  again).
- **On a node with no configuration** (disabled, or failed): the block alone is reshaped at the
  commit, no hook called; an enable later configures it with the ports the block already has.
- **In place**, when every changed key is `.inPlace()` (`NodeChange::inPlace`), the type has a
  change version, and the configuration has no companion (an engine companion, a sink, a source, a
  recording or a program; one on a resized bank would need a replacement made before break on the
  new ports, which is not built, so such a node restarts): a change in place
  ([above](#changes-in-place)). The new ports are in the engine before the change version runs; the
  old code keeps the ports it was laid out for (each
  drive names them), with the new outputs written zeros, until the commit's swap brings the code for
  the new ports; the ports that go leave the engine once that swap completed. A hold names a bank the
  running code has. The change version must give a code: one that does not fails the node `lost`,
  "configure changed the ports in place and gave no code for them". `gain`'s `channels` reshapes in
  place ([node-types.md](node-types.md#gain)); `reshape_test.cpp` proves the machine with a test type,
  channels 1 and 2 playing on through a third added and removed.
- **On the wire** a change of ports is `node.updated` with `applied: "reshape"`, `inPlace` and the
  ports added and removed ([control-port.md](control-port.md#changes-of-ports)).

### Failures

A failure tears the node's configuration down at once and keeps its block and routes; the node
stays `failed` until asked (`node.start` or `node.restart`, which retry it, a change of a key that
is not live ([retunes](#retunes)), or its removal). Recovery that retries by itself is not built yet.

| Code | When |
|---|---|
| `engine-refused` | The engine refused or lost the block or an engine companion (a card that cannot open or is unplugged, an RTP receiver lost), an engine companion was not ready by its deadline (a card that never started, an RTP receiver on a held UDP port, a pipe receiver that never ran), or the data loop is wedged |
| `start-failed` | Before the node first rested `running` or `stopped`: its `configure` failed or broke a rule, a claim was refused, a sink's consumer could not begin or did not begin within 5 s, its program could not start or did not start within 5 s, its insides said `lost`, its program ended, or the type's code threw (a hook, its code's `prepare`, a sink's consumer: [writing-a-node-type.md](writing-a-node-type.md#rules-the-framework-enforces)) |
| `lost` | After that: its insides said `lost`, its program ended ([writing-a-node-type.md](writing-a-node-type.md#programs)), or the type's code threw. At any phase: ear's own bug in one of its adapters, `internal error in <callback>: <what>` ([faults by owner](#faults-by-owner)) |

A lost **block** is final (`blockLost: true`): the node cannot be restarted (`not-restartable`,
reason `block-lost`) until it is removed and added again or ear restarts.

### Fades

Every node with an output fades its outputs in over 5 ms at every start, and out over 5 ms before
every planned silence: a stop, a restart, a disable, a removal and a failure. A card's playback
bank fades too, so its speakers never click; a sink's bank (a `file` node's) is never faded, so a
recording keeps the exact samples of every cycle its code ran, and a node whose only outputs are
sinks' banks is silenced at once, with no fade before. A lost block cannot be faded. A change in
place holds outputs silent the same way, a second gain after the fade: each held output fades to
zero over 5 ms, stays there, and fades back in at the change's end
([engine.md](engine.md#holds-and-swaps)).

### Bounds, the wedged data loop and the watchdog

Every wait the framework makes on another thread has a bound, so no node can hold the graph:

| Wait | Bound | Past it |
|---|---|---|
| A fade-out's acknowledgement | 100 ms | `ear.fade-unacknowledged`; the silence is taken unfaded |
| A drive or silence on the data loop | 5 s (judged after a grace when the whole process stalled) | `ear.barrier-overdue`, then, once, `ear.data-loop-wedged` |
| A configuration's teardown (sinks drained, its program reaped, loops finished and closed, threads stopped) | 10 s | `ear.close-overdue` (counting what is still open); the configuration is leaked, never freed, and the node moves on; if it closes later, `ear.close-late` ([teardown](#teardown-by-dependency)) |
| A hook (`configure`, `start()`, `stop()`) on the main loop | 20 ms | `ear.hook-slow`, once per node and hook (a report only) |
| A phase that lasts | 2 s | The node says what it waits for (`waitingFor`: `older-node`, `block`, `companion`, `consumer`, `loop`, `store`, `recording`, `claim`, `program`) |
| An engine companion's or a sink's consumer's readiness, from its asking (once the node's loops run) | a card 10 s, an RTP or pipe receiver 5 s, a sink's consumer 5 s | The node fails: `engine-refused` for a card or receiver, `start-failed` for a consumer ([engine.md](engine.md#readiness-deadlines)) |
| A program's start, from its asking (once every companion is ready) | 5 s | The node fails `start-failed: <name> did not start within 5 s` ([writing-a-node-type.md](writing-a-node-type.md#programs)) |
| A program's end after its teardown's `SIGTERM` | 3 s | `SIGKILL` to its process group; inside the close deadline, so it is reaped before the configuration would be leaked |
| A strand's handler | 100 ms; 1 s | `ear.loop-slow`, once per loop and configuration (a report only); past 1 s the pool starts a replacement thread, so other strands keep running |

**A wedged data loop.** Once ear judges the data loop wedged it makes no PipeWire call that could
wait on it: no block, engine companion or link is made or destroyed, and no latency is told. A new
node fails `engine-refused: the data loop is wedged`, a new route is `refused` with that detail,
and what ear would destroy is leaked. ear's stop still writes `ear.conf` and every record, then
leaves through `std::_Exit(0)` (as it does whenever anything leaked is still open:
[stop](#stop-sigint-or-sigterm)).

**The main-loop watchdog.** A 100 ms main-loop timer beats a heartbeat (and, at each beat, reads
the diagnostics counters: [engine.md](engine.md#reporting-the-counters)); the watchdog thread looks
every 250 ms and, once the heartbeat is older than 500 ms, writes `ear.main-loop-stalled` (with
the call the loop is in, when known), then `ear.main-loop-resumed` once beats return. Any stall
over 750 ms is seen. It never touches PipeWire or waits on the main loop. *Why:* PipeWire's own
object changes, and a sound card's ALSA open and close, run synchronously on ear's main loop, so
a stalled data thread or a wedged USB card can hold every node and the control port's graph
requests; the watchdog makes that visible.

### Isolation

One node's lifecycle touches only its own block's code, the audio its peers read from it, and its
own engine companions' links. What is shared and how it is kept safe:

| Shared | Kept safe by |
|---|---|
| The engine and the clock driver | A refused or lost block or engine companion fails only its node; the internal clock belongs to no node and outranks every card |
| The one data loop | `process` is bounded; ear's barriers are asynchronous invokes; a barrier past 5 s marks the loop wedged |
| The main loop | Hooks never block; every wait on another thread is asynchronous; PipeWire calls are batched per turn; the watchdog reports stalls |
| ALSA calls of a card | **Not isolated (a known limit):** ALSA's open, drop and close run on the main loop, so a USB card that wedges in the kernel can block every node until ear restarts |

### Teardown, by dependency

Tearing a configuration down (a restart, a failure, a disable, a removal) goes by dependency, and
the main loop never joins a thread:

1. Every source's writer closed (a write waiting returns at once); every loop closed to work from
   outside the node; its [claims](writing-a-node-type.md#claims) going (a claimant waits for step 5
   rather than being refused) and its own claim waits dropped; its [program](writing-a-node-type.md#programs) told to
   stop, right after the loops close to outside work; the type's `stop()` if its `start()` ran (it posts its goodbye to its loops);
   the fade; the barrier, a drive of no code (after it completes, no `process` of the code runs,
   so nothing the realtime thread can reach is freed while it can reach it); the code's `release`.
   What was sent to a loop before the node was configured is destroyed unrun.
2. Engine companions destroyed (and, for a removal, the block, only after the barrier).
3. Sinks drained and finished, each on its own thread (a sink a change replaced that is still
   draining is waited for too, counted apart), and its program reaped (its end now is no
   failure, but the end this step waits for).
4. Each loop finishes: it runs the work already posted (the goodbye), then destroys its state on
   its own thread; once every loop has, each closes, cancelling any source its state left armed
   (`ear.loop-source-left`); a `sendspin` node's worker state says goodbye as it is destroyed, on
   its own loop.
5. On the main loop: the code, then its sinks (each closed in step 3, so its consumer, which may
   hold a reference into the instance, is idle), then the state of each loop that never started
   (which may read the instance too), then the instance, then its loops (a
   dedicated loop's thread has already returned), then its sources (whose writers the instance's threads may hold), then its stores (which a
   state, a consumer or the instance may hold; an open or write still queued holds only the store's
   state), then its recordings (each closed by a job on the I/O thread, posted before its claim is
   released, so a next open of its path comes after the close), then its program (reaped in step 3, so a
   port it held is released only after its reaping) and its config freed; last, its claims released, each
   claimant that waited for one judged again in arrival order.

Steps 3 and 4 have the close deadline ([bounds](#bounds-the-wedged-data-loop-and-the-watchdog)).
It judges only a close still under way: once the last sink, loop and program reported, the
configuration has closed, and a deadline heard after that, even ahead of the node's own hearing of
it, leaks nothing and says nothing.
A restart or retry completes the teardown before the
next `configure`, so a node has one configuration at a time and never waits on its own old one.
Anything a configuration's threads tell lands only on that configuration, never on the next: each
configuration has one channel to the main loop, which every report from another thread rides (its
node handle's notes, losses and log entries, [writing-a-node-type.md](writing-a-node-type.md#the-node-handle);
a sink's begun and closed; a loop's finished and closed; a consumer's or loop
handler's throw; an I/O job's completion), and which is
detached on every path that ends the configuration (it closed, or it failed to start), before the
next can begin. That detach is the configuration's generation: nothing
queued, or said later through a kept handle, reaches the node after it. Facts (a loss, a throw)
are dropped once its teardown began; notes are heard until the detach. A stop does only step 1's
fade and silence, keeping everything else, the channel and its note rate included.

**A configuration leaked at the close deadline keeps listening.** Past the deadline it is
`ear.close-overdue` and leaked: not freed while its threads may still touch any of it, and the
node moves on (its next configuration may begin). Its channel is not detached but re-pointed at a
record ear keeps for it, so nothing it tells reaches the node or a newer configuration:

- **Still heard, on the record, in order:** each close the teardown was waiting for (a sink's,
  its program's reaping, a loop's finished and closed), and the teardown carries on
  from where it stopped (a configuration leaked while a sink drained has its loops finished and
  closed once that sink reports). A consumer's or loop handler's throw
  is a `log` record (`source` `ear`, `error`, naming the node), never a fact.
- **Dropped:** its node handle's notes, losses and log entries, its I/O jobs' completions (the
  handle a dropped completion carries is released on the I/O thread, as for any ended
  configuration), and every write of its stores not yet begun, so a thread of it never replaces
  what a newer configuration of the node keeps in the file (a `replace` it waits on is refused
  `not written: its configuration was leaked`).
- **Its claims** ([writing-a-node-type.md](writing-a-node-type.md#claims)): its ports are
  abandoned, so the next claimant probes the kernel afresh; its stores and recordings are kept,
  since its threads may still write them, refusing a claimant `<file>: held by node <n>'s earlier
  configuration, which did not close; it starts when that closes`. The record lists each node its
  claims refuse that way, and each refused by a fresh probe of a port it abandoned.
- **The late close.** Once everything it held has reported, its recordings' closes are posted to
  the I/O thread (so they come before any open of their paths that follows), its claims are released, the record
  says `ear.close-late` ([output-records.md](output-records.md#diagnostic); `; its claims are
  released` when it still held one), each node its claims refused is started again, once, as
  `node.restart` would (if that refusal is still what it rests failed on), the channel is detached, and the
  configuration no longer counts as leaked (`NodeHost::leaked()`), so a stop afterwards ends
  normally rather than through `std::_Exit` ([stop](#stop-sigint-or-sigterm)). Then, in a main-loop
  turn of its own, the configuration is freed in step 5's order: every thread of it has reported
  closed, so nothing reaches what is freed, and a node that leaks and recovers again and again
  holds nothing for it.

A configuration leaked on an overdue barrier ([bounds](#bounds-the-wedged-data-loop-and-the-watchdog))
is detached instead: the data loop may still run its code, so it never closes and counts as leaked
for good; its ports are abandoned and its stores kept for good, a claimant refused in words of its
own (`... which the wedged data loop keeps; it starts once ear restarts`), as no node is configured
again once the data loop is wedged.

## Boot and stop

### Running it

```bash
./out/ear                                                   # ./config/ear.conf; file nodes write into ./recordings
./out/ear --config-dir ~/ear-config --web-root web/dist     # another graph file folder; the editor built by make web
./out/ear --runtime-dir /run/ear                            # ear's runtime folder made inside a folder of your own
```

| Flag | Default | What |
|---|---|---|
| `--config-dir` | `config` | The graph file folder, holding `ear.conf` and each `sendspin` node's key file; made at boot if missing (not its parents). It must be writable for edits to be saved |
| `--recordings-dir` | `recordings` | The folder `file` nodes write into; made at boot if missing (not its parents) |
| `--rate` | `48khz` | The engine's rate, used only when `ear.conf` cannot be read or its `graph` block does not give both `rate` and `block_size`; written as `ear.conf` writes a rate, 8 kHz to 768 kHz ([graph-file.md](graph-file.md#rates)). The old `--graph-frequency` is refused, saying to write `--rate` |
| `--block-size` | `256` | The engine's block size, under the same rule |
| `--control-address` | `0.0.0.0` | The control port's address |
| `--control-port` | `7777` | Its port; `0` takes any free one |
| `--web-root` | `<directory of ear>/web` | The editor's built files |
| `--allowed-host` | none | A DNS name the Host header may carry (repeatable) |
| `--runtime-dir` | `$TMPDIR`, else `/tmp` | The folder ear makes its runtime folder in. The runtime folder holds programs' and pipe receivers' runtime files: each configuration's FIFOs, a program's feed and its files, each named `<node>.<configuration>.<what>` (`runtimePath`). It is always a fresh `ear-XXXXXX` (`mkdtemp`: mode 0700, ear's own) made at boot inside the folder given, which must exist and be writable; it is removed at stop, and one not empty then (a leaked configuration's files) is a `log` record, `warn`: `the runtime folder <path> was not empty at exit`. *Why always made:* a pipe receiver's FIFO is open to every writer ([engine.md](engine.md#pipe-receivers)), so only a folder whose mode ear set keeps other users out whatever the given folder's owner or mode; and a run killed before its stop leaves files whose names (`<node>.1.config`) the next run would use. The folder's path (made absolute) may hold only letters, digits and `_ . / -`, as it is written into the pipe module's arguments and a program's config unescaped; any other exits 1: `runtime folder <path>: its path may hold only letters, digits and _ . / - (...)`. Every start needs it, whether or not the graph holds a program: in a container with a read-only root, mount a writable volume at `/tmp` or give `--runtime-dir` one |

The four folders (graph file folder, recordings folder, web root, runtime folder) may not be the
same, nor inside or around each other. `ear` reads `engine.conf` from beside its own binary.

### The container image

The run image (`docker/Dockerfile`'s `run` target, published as `ghcr.io/mcurcio/ear`, amd64 only
until the Talos nodes' CPU is known; anonymous pulls work) is `debian:trixie-slim` with only the
run-time libraries: no compiler and no knobly (it is compiled in). It holds `/opt/ear/ear`,
`engine.conf`, `web/` and `licenses/` (the licences of the fetched code compiled in: knobly and
what the `sendspin` node links; and shairport-sync's, under `licenses/shairport-sync/`), and
`/usr/local/bin/shairport-sync` (built with AirPlay 2), which each `airplay` node runs
([node-types.md](node-types.md#airplay)), and not nqptp, which an AirPlay 2 node needs and the operator runs
beside ear ([node-types.md](node-types.md#airplay-2-and-nqptp)); only the dev image has nqptp, for the tests,
and `scripts/image-smoke.sh` fails a run image that holds it. It runs
as user `ear` (uid 10001), exposes 7777 and 8928 (a listening `sendspin` node's default), and
defaults to `--config-dir /var/lib/ear/config --recordings-dir /var/lib/ear/recordings`, two
volumes owned by `ear`. Arguments given to the container replace both, so pass them again.

- **The graph file folder must be writable by uid 10001.** A read-only mount (a ConfigMap) cannot
  be edited, a `sendspin` node cannot make its key file there and fails, and a first boot
  without `ear.conf` runs nothing until the editor's pane saves one.
- **Realtime where allowed.** The binary carries `CAP_SYS_NICE` as a permitted-only file
  capability that ear makes effective at boot, so the audio thread is realtime where the
  container is given `SYS_NICE` (`docker run --cap-add SYS_NICE`) and plain elsewhere; ear prints
  which, and `graph.get` says it ([engine.md](engine.md#realtime-priority)).

Running it on Talos with the MOTU 24Ao is the `ear-on-talos` skill's.

### Boot

1. Read the flags. Bad flags and `--help` are plain text; from here on every line is a record.
   Then, before any thread starts, block SIGCHLD (every thread inherits it blocked; only the
   programs' thread hears it, through its signalfd) and make ear its descendants' subreaper
   (`PR_SET_CHILD_SUBREAPER`): a process a program leaves behind is re-parented to ear, whether
   or not ear is PID 1, and reaped by the programs' thread, never left a zombie. A program also dies
   with ear: it runs under `setpriv --pdeathsig KILL --nnp`.
2. Capture descriptors 1 and 2 ([output-records.md](output-records.md#how-output-is-written)).
3. Open the two folders, make the runtime folder, and check the four are apart; a failure
   is an `exit` record and exit 1.
   Every declared store's files in the graph file folder are narrowed to their stores' modes
   ([writing-a-node-type.md](writing-a-node-type.md#stores)); what cannot be is a `log` record.
4. Read `ear.conf` once on the main thread. Its `graph` block's `rate` and `block_size` set the engine
   when both read; otherwise the flags do (the file's own values then show as pending restart).
5. Start the engine and print how its audio thread is scheduled (`engine` record).
6. Wire the packages and start the keeper's thread. Its first offer (posted once the main loop
   runs) plans the file from an empty graph. A file that is missing, has errors, or that the graph refuses runs nothing: ear stays
   up and the editor's pane shows why until the file is fixed.
7. Open the control port (`control` record).

Exit 1 comes only from bad flags, a folder it cannot make or open (a `--runtime-dir` that does
not exist, or whose path holds a character outside `[A-Za-z0-9_./-]`, among them), folders that overlap, node types
the catalogue refuses (two of one name, or a name not 1 or more of `[a-z0-9]`), an
engine that does not start, a folder watch it cannot arm, or a control port it cannot bind.

### Stop (SIGINT or SIGTERM)

1. `DesiredGraph::close()`: every later edit is refused `stopping`.
2. The keeper is told to stop: edits still queued get `stopping`; reads are still answered. Its
   thread removes its folder watch and closes the inotify descriptor (a stopped keeper takes no
   disk event).
3. `NodeHost::stopAll` takes every node through `stopping` (cause `removing`) and tears it down
   (a `file` node finishes its WAV file).
4. The control port closes every client with WebSocket code 1001; the control thread stops; the
   main loop quits.
5. The main thread joins the keeper's thread, which first writes the last accepted version of
   `ear.conf` if it was not written yet (a failure is a `graph-file-write-failed` record; a hung
   disk delays exit without limit).
6. The I/O thread stops: jobs still queued are dropped unrun, and the one in flight gets 2 s. One
   still running then is a `log` record (`source` `ear`, `warn`:
   `the I/O thread was still busy at exit: <job>`), and its thread is never waited for. Then the
   runtime folder ear made is removed (one not empty is a `log` record, above).
7. ear writes its last record, `exit` with status 0 (`ear: stopped`,
   [output-records.md](output-records.md#exit)).
8. Every queued record is written; ear exits 0 (through `std::_Exit(0)` if anything leaked is still open,
   or the I/O thread was still busy; a configuration leaked at the close deadline that closed late,
   `ear.close-late`, no longer counts: [teardown](#teardown-by-dependency)).

*Why stop in this order:* recordings are finished before anything waits on the disk, and the
last `ear.conf` write happens before Kubernetes' grace period ends whatever the data loop or a
card does. The I/O thread stops last because every node, and the keeper, may post to it until
they are gone; a hung disk under its job delays exit by 2 s at most.

*Why the descriptor closes at step 2:* closing an inotify descriptor waits for the kernel's mark
reaper, a delayed work item and an RCU grace period that is host-wide: the close waits on any
process's pending marks, some 16 ms on an idle host and whole seconds on a starved one. Closed on
the keeper's thread as the stop begins, that wait overlaps the nodes' stop, and step 5's join takes
what is left of it, before `ear: stopped`; nothing after the record waits on it. An exit 1 after
the keeper started (a control port it cannot bind) stops and joins the keeper before its `exit`
record for the same reason. What follows the record is ear's destructors and the kernel's teardown
of the process: the record, not the process's end, says when ear's stop is done.

## Not built yet

These are designed or wanted but not in the code: routes naming a whole bank; a reshape in
place of a configuration with companions ([reshapes](#reshapes): the companions on resized inside banks would be
replaced make-before-break, each new one handed over in one invoke, so a bank never holds two at once); recovery of failed nodes; sound cards opened off the main loop; an internal
clock anchored to one card; serving PipeWire's socket (for a program that can only be a PipeWire client; its nodes would be engine companions, marked by the socket's peer credentials); a login on port 7777; automatic card retry and hotplug; `@include`; undo; changing the graph
frequency or block size while ear runs; arm64 images (which wait on a port added to a running block on the data loop: [engine.md](engine.md#the-one-data-loop-and-invokes)); a fault caught at the callback boundary
failing only the node that owns it (today it ends the process); the `sendspin` library's log lines attributed to their node;
and the converter companion's set and tracked modes ([the timing model](#the-timing-model)). The design pages for the unbuilt slices
remain in `docs/design/`.
