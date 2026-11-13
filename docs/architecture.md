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

## Packages

Each C++ package lives in `src/<package>/` with its own `CMakeLists.txt`, public headers under
`src/<package>/include/ear/<package>/` and tests in `tests/`. A node type lives in
`src/nodes/<type>/` and is its own library ([writing-a-node-type.md](writing-a-node-type.md)).

| Target | Owns | Never |
|---|---|---|
| `ear_core` (`src/core`) | The desired graph (`DesiredGraph`: the one judge of every change, `plan` then `commit`); the node type contract (`NodeType`, settings tables, `TypeSpec`, the type's class and its contexts); the node framework (`NodeHost`, the node lifecycle machines, health, `NodeOperations`, the pass-through, interim sinks, latency plans); the reconciler (route links and route status); levels; the graph read (`GraphStatus`); `FrameRing`. Beside it, for tests only, `ear_type_harness` (`ear/testing/`) | Any PipeWire call, socket, thread or file access. nlohmann/json is its only dependency |
| `ear_folder` (`src/folder`) | `ConfinedFolder`: a folder held open from boot, every path resolved beneath it by the kernel (`openat2` with `RESOLVE_BENEATH`); files replaced only if unchanged (`renameat2(RENAME_EXCHANGE)`); `requireApart`, the boot check that no two folders overlap | Following `..`, absolute paths or (for the graph file) any symlink |
| `ear_node_white`, `ear_node_file`, `ear_node_card`, `ear_node_rtp`, `ear_node_sendspin` (`src/nodes/<type>`) | One built-in node type each ([node-types.md](node-types.md)) | Another node type; PipeWire (only `ear_core`'s interfaces) |
| `ear_all_node_types` (`src/all_node_types`) | `allNodeTypes()`, the one list of node types, generated at configure from the folders in `src/nodes/` | Naming a type by hand |
| `ear_pipewire` (`src/pipewire`) | The in-process engine and the adapters that implement `ear_core`'s interfaces: port blocks, links, live ports, sinks and handoffs, engine companions (sound cards, module-run converters: RTP receivers), the main-loop executor, the engine's log ([engine.md](engine.md)); the callback boundary every C callback ear gives PipeWire goes through ([below](#the-callback-boundary)) | Policy; a C callback registered outside the boundary |
| `ear_output` (`src/output`) | Where standard output goes: the output thread, the capture of descriptors 1 and 2, the main-loop watchdog ([output-records.md](output-records.md)) | A record's text |
| `ear_control` (`src/control`) | Standard output's one encoder (`StatusLog`); the JSON-RPC protocol (envelope, decoding, commands, events, sessions); the wire types of the graph file and the lifecycle; `MainLoopGateway`, the one crossing between the main loop and the control thread | Sockets; graph rules; knobly |
| `ear_config` (`src/config`) | The graph file keeper: `ear.conf`'s model through knobly, the keeper's thread (working copy, folder watch, writes) and its main-loop side (plan and commit, the running version) | Judging a graph; letting a knobly type leave its thread |
| `ear_server` (`src/server`) | The control thread and port 7777 (Boost.Beast): the editor's files, `/rpc`, Host and Origin checks, per-client queues, the level bell | Graph state; PipeWire |
| `ear` (`src/main.cpp`) | The composition root: wiring, boot and stop order, and glue between packages that do not see each other | Computing anything |
| `web/` | The graph editor (React, React Flow, Vite; [editor.md](editor.md)) | Graph rules: it draws only what ear announced and never parses knobly |

**Dependency rules.** Dependencies point one way: `ear_core` is below everything; the node types,
`ear_pipewire`, `ear_control` and `ear_config` depend on it; `ear_control` is below `ear_server`
and `ear_config`; `ear_folder` is below `ear_node_sendspin` and `ear_config`; `ear` links all of
them, and links the node types only through `ear_all_node_types`. No node type links another
(configure refuses it). `ear_output` and `ear_folder` depend on nothing of ear's. No Boost,
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
| PipeWire's data loop (realtime, one for the whole engine) | Every port block's `process`, level metering, the sinks' taps and the legacy handoffs' writers; every engine companion's processing |
| PipeWire's main loop (the main thread) | Every piece of graph state: the engine, desired graph, node framework, reconciler, adapters, the gateway's graph side, the keeper's main-loop side; `ear.conf`'s first read at boot |
| The control thread | Port 7777: sockets, JSON-RPC, sessions, queues. It holds only the gateway, which sends every request whole to the keeper and gets replies and events back in order from the main loop |
| The keeper thread | The graph file keeper: every request, disk check and write, one step at a time; the working copy (knobly's object, which never leaves this thread); the inotify watch. It waits in `poll()` with no timeout |
| Sink and handoff threads | A `file` node's sink (its WAV writer); a `sendspin` node's legacy handoff |
| The output thread | Every write to the real standard output, from a bounded queue |
| The capture thread | Reads the pipes on descriptors 1 and 2 into `log` records, and drains the engine's log ring |
| The watchdog thread | `StallWatch`: watches the main loop's heartbeat ([below](#bounds-the-wedged-data-loop-and-the-watchdog)) |
| A `sendspin` node's threads | Its worker, its notifier and the protocol library's own ([node-types.md](node-types.md#sendspin)) |

Everything is moved by events, not polling, with one exception: a `sendspin` node's worker polls
its library every 10 ms and watches its connection with 5 s and 31 s timers, because the library
has no wake hook and no event for a lost connection or a failed dial. Those timers stay inside
that thread.

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
  (on loops that may run anything: the main loop, a sink's or handoff's thread loop) and an invoke
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
- **The guard.** The CTest test `ear_callbacks_through_the_boundary`
  (`test/pipewire/callback_guard.cmake`) fails on any file under `src/` that names a way to hand a
  library a callback itself: a loop source, invoke or locked call (`pw_loop_add_*`,
  `spa_loop_add_source`, `spa_loop_utils_add_*`, `pw_loop_invoke`, `pw_data_loop_invoke`,
  `spa_loop_invoke`, `pw_loop_locked`, `spa_loop_locked`, `pw_work_queue_add`), a listener (any
  `*_add_listener`, `*_add_object_listener` or `*_add_rt_listener`, its parenthesis on the same line
  or not, or `spa_hook_list_append`), a stream or filter made with its events (`pw_stream_new_simple`,
  `pw_filter_new_simple`), an interface implemented (`SPA_INTERFACE_INIT`, `SPA_CALLBACKS_INIT`,
  `spa_callbacks`, a `spa_*_methods` or `pw_*_methods` table), `pw_log_set` or
  `snd_lib_error_set_handler`. Exempt are exactly `pipewire/callback_boundary.hpp` and `.cpp` and
  tests, which drive PipeWire directly. The guard is tested on each spelling it refuses, on
  look-alikes, on a file merely named like the boundary, and on having nothing to check.
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
placed in that call. Off the main thread (a sink's or legacy handoff's thread) only the callback
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
catch of its own, before any boundary: its hooks, its code's `prepare` and a legacy recipe and
close on the main loop, and a sink's or legacy handoff's consumer on that one's thread. What a type
throws there, a `std::logic_error` included, is a fact about its node
([writing-a-node-type.md](writing-a-node-type.md#rules-the-framework-enforces)), and the rest of the
graph runs on. A type's code run at parse (its settings' parse functions, `check`, `ports`) goes
through the same catch in `NodeType::parse`, and what it throws refuses the edit as
`invalid-settings`. So a throw that leaves NodeHost, or a sink's or handoff's own code, for the
boundary is ear's own bug, never a type's.

### Faults by owner

ear's own bugs are split by who owns the state the bug may have left half-changed. *Why:* an
adapter's state dies with its node, so failing only that node is clean and the rest of the graph
plays on; NodeHost's state is every node's, and its invariants cannot be trusted after an exception
in the middle of an update, so ending the process (the container restarts it) stays the safe
answer there. Every caught bug is said in an `ear.internal-error` record (`callback`, `node` when
it fails one, `fatal` when ear ends after it, `detail`;
[output-records.md](output-records.md#diagnostic)), once per owner: an owner whose callbacks keep
throwing is said and told only the first time (`FaultOwner::firstFault`).

| Where it was thrown | Owner | What follows |
|---|---|---|
| A type's code: its hooks, its code's `prepare`, a legacy recipe or close, a sink's or handoff's consumer, its parse | The type | Its node's fact (`start-failed`, or `lost` once rested; an edit refused at parse) and no record ([writing-a-node-type.md](writing-a-node-type.md#rules-the-framework-enforces)) |
| An adapter's own callback: a port block's, an engine companion's or its body's (a card's, an RTP receiver's), a link's | The adapter (`FaultOwner`) | The record, with the node; the adapter gives up what it holds and the node fails `lost`, `internal error in <callback>: <what>`. A block is lost for good (`blockLost`) and leaves the graph at once (its listener, the process callback with it, then, outside the filter's events, the filter itself), so every request its node's teardown makes is applied on the main thread and completes, and no silence can be judged a wedged data loop; an engine companion is torn down as on its loss, from wherever its callback left it, and a retry makes it afresh. A route's link fails its route, `refused` with that detail, and names no node |
| An adapter NodeHost calls inside a type's context call (`SinkAdapter::make`, `SourceAdapter::make`, `HandoffAdapter::make`, `RecordingAdapter::open`): a `std::logic_error`, or nothing made where it must make something | The adapter | The record, with the node and where (`configure <node>`); the node fails `lost` even if the type caught the throw |
| The same call's refusal (any other exception: no thread, no eventfd, no memory), through any context call it is nested in (a recording through the legacy start's `acquire`), and `std::bad_alloc` from NodeHost's own code there | Not a bug | The node's `start-failed`, `configure threw: <what>`, even if the type caught it; no record |
| A port block factory or an engine companion adapter, in the flush (each refuses through its events, never by a throw) | The adapter | The record, with the node and its flush item; the node fails `lost` (the block lost for good). Safe to run on because every adapter constructor is strongly exception-safe by construction (below) |
| NodeHost itself, or any adapter call that interrupts NodeHost's own update (a block's `drive` or `carryLatency` as NodeHost carries out its actions), anything of core's an adapter's callback tells (a route link's listeners: the reconciler, and through it standard output and the gateway), the main loop's posted work and timers, the engine's signals, the live graph's registry, a sink's or handoff's own thread | No owner but ear's core | The record without a node, `fatal`, then the line and `std::terminate` (above) |

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
as core's, whichever callback it came out of, and does not tell the owner. A sink's or legacy
handoff's own code runs on its own thread, where no node can be failed in order with the rest, so
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
   *candidate* ([graph-file.md](graph-file.md#edits-at-the-source)).
2. The keeper posts the candidate to the main loop, where its main-loop side asks
   `DesiredGraph::plan` for a verdict against the whole wanted graph.
3. If accepted, in one main-loop turn: the file change is told (gateway, then `StatusLog`), the
   plan is committed, its node and route events follow, and the reply is sent.
4. The keeper keeps the candidate as its working copy and writes `ear.conf` once no step is
   waiting (write-behind): it swaps the new text in and keeps it only if the text it replaced is
   what it last read or wrote.
5. A refused candidate is dropped; nothing is told or written, and only the reply carries the
   refusal.

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
   (sound cards, RTP receivers, recordings, sinks) and gives the code that will run. Once every
   resource is ready and the block is ready, the framework drives the block with that code
   ([the node lifecycle](#the-node-lifecycle)).
4. **Each cycle**, on the realtime thread, the block calls the code with one span per port and a
   `GraphCycle` (the cycle's start on both monotonic clocks, the driver's position, rate and id,
   the next cycle's start, an xrun count, whether the clock jumped, and the latency downstream of
   each output), so no type reads a clock ([engine.md](engine.md#graphcycle)).
5. **Routes.** `LiveGraph` reports ports as the engine's registry announces them; the
   `Reconciler` links wanted routes between live ports. An unlinked input reads zeros.
6. **Engine companions.** A `card` or `rtp` node's block has inside banks no route can name; the
   framework joins an ALSA adapter node or an RTP receiver to them, and its pass-through copies
   between inside and public banks ([engine.md](engine.md#engine-companions)).
7. **Off the realtime thread.** A `file` node's block writes each cycle into a sink's lock-free
   ring, read by the sink's own thread ([engine.md](engine.md#sinks)).
8. **Levels.** Each block meters its public ports and publishes a window every 1/30 s of graph
   time; the control thread sends the newest to subscribers
   ([control-port.md](control-port.md#levels)).

## The timing model

ear's audio runs on one tick at one rate, and every other clock belongs to the node at the
graph's edge that brings it in. *Why:* the owner's model, after `sendspin` showed that time the
framework keeps for a node is kept again by whatever the node talks to, and the two correct the
same lost time.

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
   `sendspin` does it in its code ([node-types.md](node-types.md#clock)); a type's source does it
   in the `SourceClock` it gives the source, which alone decides what the ring skips
   ([writing-a-node-type.md](writing-a-node-type.md#a-sources-clock)).
4. **One rate inside the graph.** Every port in the graph runs at the graph rate, and conversion
   happens only in edge nodes. The rate is configurable (`graph_frequency` in `ear.conf`, the
   `--graph-frequency` flag as fallback: [boot](#boot)) but fixed for the engine's life, and it is
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

**Where the code still departs**, each removed by
[`design/timing-model.md`](design/timing-model.md):

- `sendspin`'s clock follows the model (an honest sink with a local skip of lost time, its
  reports deferred from overdue ticks under a 2 s bound, with `sendspin-starved-floor.patch`:
  [node-types.md](node-types.md#clock)), but in the node's own code over its own feed, not yet as
  the `SourceClock` of a source ([writing-a-node-type.md](writing-a-node-type.md#a-sources-clock)),
  which comes when the node moves off the old interface.
- The converter companion has only its module-run mode
  ([engine.md](engine.md#module-run-converters)), whose one user is the RTP receiver: there is no
  set or tracked converter yet for a ring whose clock is a type's own, so only cards and RTP
  receivers convert.
- `sendspin` is not yet a source and a sink: its audio crosses through its own feed and a legacy
  handoff ([writing-a-node-type.md](writing-a-node-type.md#the-old-interface)) until it moves
  onto the type's class.

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
of a node's ports (a card's channel count) or of its type replaces the node: its routes and the
node are removed and added again. A change to a disabled node is stored for when it is enabled.

### Failures

A failure tears the node's configuration down at once and keeps its block and routes; the node
stays `failed` until asked (`node.start` or `node.restart`, which retry it, a change of settings,
or its removal). Recovery that retries by itself is not built yet.

| Code | When |
|---|---|
| `engine-refused` | The engine refused or lost the block or an engine companion (a card that cannot open or is unplugged, an RTP receiver lost), an engine companion was not ready by its deadline (a card that never started, an RTP receiver on a held UDP port), or the data loop is wedged |
| `start-failed` | Before the node first rested `running` or `stopped`: its `configure` failed or broke a rule, a sink's consumer could not begin or did not begin within 5 s, its insides said `lost`, or the type's code threw (a hook, its code's `prepare`, a sink's consumer: [writing-a-node-type.md](writing-a-node-type.md#rules-the-framework-enforces)) |
| `lost` | After that: its insides said `lost`, or the type's code threw. At any phase: ear's own bug in one of its adapters, `internal error in <callback>: <what>` ([faults by owner](#faults-by-owner)) |

A lost **block** is final (`blockLost: true`): the node cannot be restarted (`not-restartable`,
reason `block-lost`) until it is removed and added again or ear restarts.

### Fades

Every node with an output fades its outputs in over 5 ms at every start, and out over 5 ms before
every planned silence: a stop, a restart, a disable, a removal and a failure. A card's playback
bank fades too, so its speakers never click; a sink's bank (a `file` node's) is never faded, so a
recording keeps the exact samples of every cycle its code ran. A lost block cannot be faded.

### Bounds, the wedged data loop and the watchdog

Every wait the framework makes on another thread has a bound, so no node can hold the graph:

| Wait | Bound | Past it |
|---|---|---|
| A fade-out's acknowledgement | 100 ms | `ear.fade-unacknowledged`; the silence is taken unfaded |
| A drive or silence on the data loop | 5 s (judged after a grace when the whole process stalled) | `ear.barrier-overdue`, then, once, `ear.data-loop-wedged` |
| A configuration's teardown (sinks drained, threads stopped) | 10 s | `ear.close-overdue`; the configuration is leaked, never freed, and the node moves on |
| A hook (`configure`, `start()`, `stop()`) on the main loop | 20 ms | `ear.hook-slow`, once per node and hook (a report only) |
| A phase that lasts | 2 s | The node says what it waits for (`waitingFor`: `older-node`, `block`, `companion`, `consumer`) |
| An engine companion's or a sink's consumer's readiness, from its request | a card 10 s, an RTP receiver 5 s, a sink's consumer 5 s | The node fails: `engine-refused` for a card or receiver, `start-failed` for a consumer ([engine.md](engine.md#readiness-deadlines)) |

**A wedged data loop.** Once ear judges the data loop wedged it makes no PipeWire call that could
wait on it: no block, engine companion or link is made or destroyed, and no latency is told. A new
node fails `engine-refused: the data loop is wedged`, a new route is `refused` with that detail,
and what ear would destroy is leaked. ear's stop still writes `ear.conf` and every record, then
leaves through `std::_Exit(0)` (as it does whenever anything was leaked).

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

1. Every source's writer closed (a write waiting returns at once); the type's `stop()` if its
   `start()` ran; the fade; the barrier, a drive of no code (after it completes, no `process` of
   the code runs, so nothing the realtime thread can reach is freed while it can reach it); the
   code's `release`.
2. Engine companions destroyed (and, for a removal, the block, only after the barrier).
3. Sinks and handoffs drained and finished, each on its own thread.
4. A legacy run object closed, off the main loop (a `sendspin` node's worker says goodbye on its
   own thread).
5. On the main loop: the code, then the instance (or run object), then its sinks and sources
   (whose writers the instance's threads may hold) and its config freed, and recordings closed
   after them.

Steps 3 and 4 have the close deadline ([bounds](#bounds-the-wedged-data-loop-and-the-watchdog)).
A restart or retry completes the teardown before the
next `configure`, so a node has one configuration at a time and never waits on its own old one.
Anything a configuration's threads tell lands only on that configuration (it carries an internal
generation), never on the next, and facts are dropped once its teardown began. A stop does only
step 1's fade and silence, keeping everything else.

## Boot and stop

### Running it

```bash
./out/ear                                                   # ./config/ear.conf; file nodes write into ./recordings
./out/ear --config-dir ~/ear-config --web-root web/dist     # another graph file folder; the editor built by make web
```

| Flag | Default | What |
|---|---|---|
| `--config-dir` | `config` | The graph file folder, holding `ear.conf` and each `sendspin` node's key file; made at boot if missing (not its parents). It must be writable for edits to be saved |
| `--recordings-dir` | `recordings` | The folder `file` nodes write into; made at boot if missing (not its parents) |
| `--graph-frequency` | `48000` | The engine's rate, used only when `ear.conf` cannot be read or does not give both `graph_frequency` and `block_size` |
| `--block-size` | `256` | The engine's block size, under the same rule |
| `--control-address` | `0.0.0.0` | The control port's address |
| `--control-port` | `7777` | Its port; `0` takes any free one |
| `--web-root` | `<directory of ear>/web` | The editor's built files |
| `--allowed-host` | none | A DNS name the Host header may carry (repeatable) |

The three folders (graph file folder, recordings folder, web root) may not be the same, nor
inside or around each other. `ear` reads `engine.conf` from beside its own binary.

### The container image

The run image (`docker/Dockerfile`'s `run` target, published as `ghcr.io/mcurcio/ear`, amd64 only
until the Talos nodes' CPU is known; anonymous pulls work) is `debian:trixie-slim` with only the
run-time libraries: no compiler and no knobly (it is compiled in). It holds `/opt/ear/ear`,
`engine.conf`, `web/` and `licenses/` (the licences of what the `sendspin` node compiles in), runs
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
2. Capture descriptors 1 and 2 ([output-records.md](output-records.md#how-output-is-written)).
3. Open the two folders and check the three are apart; a failure is an `exit` record and exit 1.
4. Read `ear.conf` once on the main thread. Its `graph_frequency` and `block_size` set the engine
   when both read; otherwise the flags do (the file's own values then show as pending restart).
5. Start the engine and print how its audio thread is scheduled (`engine` record).
6. Wire the packages and start the keeper's thread. Its first offer (posted once the main loop
   runs) plans the file from an empty graph. A file that is missing, has errors, or that the graph refuses runs nothing: ear stays
   up and the editor's pane shows why until the file is fixed.
7. Open the control port (`control` record).

Exit 1 comes only from bad flags, a folder it cannot make or open, folders that overlap, an
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
6. ear writes its last record, `exit` with status 0 (`ear: stopped`,
   [output-records.md](output-records.md#exit)).
7. Every queued record is written; ear exits 0 (through `std::_Exit(0)` if anything was leaked).

*Why stop in this order:* recordings are finished before anything waits on the disk, and the
last `ear.conf` write happens before Kubernetes' grace period ends whatever the data loop or a
card does.

*Why the descriptor closes at step 2:* closing an inotify descriptor waits for the kernel's mark
reaper, a delayed work item and an RCU grace period that is host-wide: the close waits on any
process's pending marks, some 16 ms on an idle host and whole seconds on a starved one. Closed on
the keeper's thread as the stop begins, that wait overlaps the nodes' stop, and step 5's join takes
what is left of it, before `ear: stopped`; nothing after the record waits on it. An exit 1 after
the keeper started (a control port it cannot bind) stops and joins the keeper before its `exit`
record for the same reason. What follows the record is ear's destructors and the kernel's teardown
of the process: the record, not the process's end, says when ear's stop is done.

## Not built yet

These are designed or wanted but not in the code: routes naming a whole bank; changing a node's
settings in place without a restart (a type's change version of `configure` compiles but is
never called); a sink's realtime side as its own engine node, and companions on the wire; `sendspin` on the type's class (it still runs through
the legacy adapter); recovery of failed nodes; sound cards opened off the main loop; an internal
clock anchored to one card; serving PipeWire's socket, a program supervisor and the `airplay`
type; a login on port 7777; automatic card retry and hotplug; `@include`; undo; changing the graph
frequency or block size while ear runs; arm64 images; a fault caught at the callback boundary
failing only the node that owns it (today it ends the process); a source whose clock is a type's own on any shipped type,
and the converter companion's set and tracked modes ([the timing model](#the-timing-model)). The design pages for the unbuilt slices
remain in `docs/design/`.
