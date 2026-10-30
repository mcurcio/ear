# Milestone 1 (white noise → file sink): module design, frozen interfaces, slice cut
Source: code architect, 2026-10-29. Owner answers (gate page, 2026-10-29): container base Debian 13; remove JUCE in this milestone (slice B). System design: docs/design/pipewire-graph.md and the design page.
Owner gate page: https://claude.ai/artifact/9rHQzmUcx6kdhguft7LBtP. PipeWire
sources fetched: scratchpad/pw-m1/, pwsrc/, pwsrc2/. [V] = verified in source, [U] = unverified
(each [U] is an acceptance criterion).

## 1. Module map
| Target (dir) | Context | Owns | Never |
|---|---|---|---|
| ear_core (src/core) | Desired graph | wanted nodes/routes, route checks vs declared ports, CommandError | what exists |
| ear_core | Node catalogue contract | NodeType, NodeConfig, AudioCode, handoff ends, type lookup | when nodes exist |
| ear_core | Node framework | each node's state machine; everything a node made; teardown order (port block → handoffs → insides) | routes; any PipeWire call (via factory interfaces) |
| ear_core | Reconciler | route links and route status | node state; audio |
| ear_nodes (src/nodes) | Built-in types | white, file | PipeWire (uses libsndfile only) |
| ear_pipewire (src/pipewire) | Engine | in-process engine; implements core interfaces: port blocks (pw_filter), links (link-factory), live ports (registry), handoffs (thread loop) | policy |
| ear_control (src/control) | Control surface (M1) | reads ear.json v2, replays as addNode/addRoute; status lines | graph rules; engine calls |
| ear (src/main.cpp) | Composition root | wiring | computing anything |
Dependencies: ear_core ← {ear_nodes, ear_pipewire, ear_control} ← ear. ear_core needs only
nlohmann/json. ear_nodes links sndfile. ear_pipewire links libpipewire-0.3 (pkg-config).
Interfaces live with their consumer (node_host.hpp declares PortBlockFactory, HandoffFactory;
reconciler.hpp declares LinkMaker and listeners). Threading: all core objects on the engine's
main-loop thread; AudioCode::process and RtWriter::write on the realtime thread;
HandoffConsumer on the handoff's own thread.
Data: ear.json → parseGraphFile → replay → DesiredGraph. nodeAdded → NodeHost (config.start(grant)
→ prepare → PortBlockFactory.make). routeAdded → Reconciler. LiveGraph.portAppeared → Reconciler
→ Links.createLink → linkActive → linked. RT: file AudioCode → RtWriter.write → ring → wake →
handoff thread → consume → sf_writef_float.
Handoff: JACK disk-writer pattern; spa_ringbuffer of interleaved floats; one pw_thread_loop per
handoff; RT wakes it with pw_loop_signal_event [U safe-on-RT; analogy filter.c:1003-1011
pw_loop_invoke block=false]. Full ring → drop and count; reported when node is gone.
Engine facts [V]: pw_context_connect_self works when not a daemon (module-protocol-native.c:
126-128,1270-1300; 1.0.5 core.c:442-452). No socket/runtime dir unless core.daemon
(:1705-1713,1894-1898,:774-785). pw_context_new loads config's context.spa-libs/modules/objects
(1.0.5 context.c:346-360). Config context.properties OVERWRITE pw_context_new props (context.c:245,
conf.c:986-990) → engine.conf must not set default.clock.*. support.dbus=false skips D-Bus
(context.c:315). No session manager needed: unlinked always-process nodes get first active driver
(1.0.5 context.c:1222,1236-1311); ≥1.7.0 moved into module-scheduler-v1. pw_filter exports a
local node via core (filter.c:1682-1713), want-driver default (filter.c:1241). Output-only filter
as source = upstream audio-dsp-src.c. pw_filter_get_dsp_buffer NULL when no buffer
(filter.c:2038-2045) → adapter gives zeros for inputs / scratch for outputs. link-factory takes
port global ids; link owned by creating client unless object.linger (1.0.5 module-link-factory.c:
263-277,365-400). Registry globals carry only whitelisted keys (impl-node.c:59-78; impl-port.c:
1016-1033) → custom mark key never reaches registry → M1 recognises EAR nodes by node.name (safe
only while no other client can connect: no socket).

## 2. Frozen interfaces (C++23; fallback C++20 + vendored tl::expected, same signatures)
Stateful classes use a private Impl pointer.

```cpp
// ===== ear_core: src/core/include/ear/core/graph_spec.hpp =====
#pragma once
#include <compare>
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace ear {
using NodeName = std::string;           // unique in the graph; also the engine node.name
using PortName = std::string;           // declared by the node type: "FL", "FR", "MONO"
using NodeSettings = nlohmann::json;    // unchecked; only NodeType::parse reads it
enum class Direction { input, output };
struct PortRef { NodeName node; PortName port;
  friend auto operator<=>(const PortRef&, const PortRef&) = default; };
/// One output port to one input port.
struct Route { PortRef from; PortRef to;
  friend auto operator<=>(const Route&, const Route&) = default; };
struct NodeSpec { NodeName name; std::string type; NodeSettings settings; // {} when absent
  friend bool operator==(const NodeSpec&, const NodeSpec&) = default; };
struct GraphSpec {
  std::uint32_t graphFrequency;  // Hz, > 0, read at boot (R5)
  std::uint32_t blockSize;       // samples per cycle, > 0, read at boot (R4)
  std::vector<NodeSpec> nodes;
  std::vector<Route> routes;
};
}

// ===== command_error.hpp =====
#pragma once
#include <string>
#include <string_view>
#include <variant>
#include "ear/core/graph_spec.hpp"
namespace ear {
struct UnknownNodeType { std::string type; };
struct InvalidSettings { NodeName node; std::string detail; };
struct DuplicateNode   { NodeName node; };   // name in use with different settings
struct UnknownNode     { NodeName node; };   // a route names a node not in the graph
struct UnknownPort     { PortRef port; };    // node exists, port not declared
struct WrongDirection  { Route route; };     // from is not an output, or to is not an input
using CommandError = std::variant<UnknownNodeType, InvalidSettings, DuplicateNode,
                                  UnknownNode, UnknownPort, WrongDirection>;
/// "unknown-node-type" | "invalid-settings" | "duplicate-node" | "unknown-node" |
/// "unknown-port" | "wrong-direction". The one home of these strings.
std::string_view errorCode(const CommandError& error) noexcept;
/// Code plus what it names, e.g. "unknown-port rec:FX".
std::string describe(const CommandError& error);
}

// ===== node_type.hpp =====
#pragma once
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "ear/core/command_error.hpp"
namespace ear {
/// Invariant: names unique across inputs and outputs (DesiredGraph::addNode throws
/// std::logic_error otherwise: a type bug, not a user error).
struct Ports { std::vector<PortName> inputs; std::vector<PortName> outputs; };
/// Why a node is failed. Part of status; never returned by a control call.
struct FailureReason { enum class Code { engineRefused, startFailed }; Code code; std::string detail; };
std::string_view failureCode(FailureReason::Code code) noexcept;  // "engine-refused" | "start-failed"
struct AudioFormat { std::uint32_t rate; std::uint32_t maxBlock; };   // fixed at boot in M1

class AudioCode {
public:
  virtual ~AudioCode() = default;
  /// Main thread, before the first process(), never concurrently with it. May allocate.
  virtual void prepare(const AudioFormat& format) = 0;
  /// Realtime: no locks, allocation, file/network access or waiting. inputs/outputs are in
  /// declaration order, each span exactly `frames` long, frames <= maxBlock. An unlinked
  /// input reads zeros. Every output sample must be written.
  virtual void process(std::span<const std::span<const float>> inputs,
                       std::span<const std::span<float>> outputs,
                       std::uint32_t frames) noexcept = 0;
  /// Main thread, after the last process() returned.
  virtual void release() noexcept = 0;
};
/// Realtime end of a handoff. Valid until the node's port block is gone.
class RtWriter {
public:
  /// Realtime thread only. Interleaves `frames` frames of planar `channels` (size == the
  /// handoff's channel count, each span >= frames) into the ring. Never blocks, locks or
  /// allocates. Returns frames accepted; the rest are dropped and counted by the framework.
  virtual std::uint32_t write(std::span<const std::span<const float>> channels,
                              std::uint32_t frames) noexcept = 0;
protected:
  ~RtWriter() = default;
};
/// Ordinary-thread end of a handoff, implemented by the node type.
class HandoffConsumer {
public:
  virtual ~HandoffConsumer() = default;
  /// The handoff's own thread (never realtime or main). May block on I/O. In write order;
  /// interleaved.size() == frames * channels.
  virtual void consume(std::span<const float> interleaved, std::uint32_t frames) = 0;
  /// Same thread, exactly once, after the last consume().
  virtual void finish() = 0;
};
/// The type's own objects for one node. Destroyed by the framework last.
class NodeInsides {
public:
  virtual ~NodeInsides() = default;
  /// Drives this node's one port block (the framework makes it; the type cannot make two
  /// or none). Same object every call; lives as long as the insides.
  virtual AudioCode& audioCode() = 0;
};
/// Powers granted to one starting node. Everything made here belongs to the node and is torn
/// down by the framework in order: port block, handoffs, insides.
class NodeGrant {
public:
  virtual AudioFormat format() const = 0;   // the value prepare() will get
  /// `consumer` must live in the returned NodeInsides.
  virtual RtWriter& handoff(std::uint32_t channels, std::uint32_t capacityFrames,
                            HandoffConsumer& consumer) = 0;
protected:
  ~NodeGrant() = default;
};
/// One node's checked settings; made only by its type's parse().
class NodeConfig {
public:
  virtual ~NodeConfig() = default;
  virtual const Ports& ports() const = 0;   // pure; the only place this node's ports are declared
  /// Main thread. Error: the node becomes failed with this reason (startFailed).
  virtual std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& grant) const = 0;
};
class NodeType {
public:
  virtual ~NodeType() = default;
  virtual std::string_view name() const = 0;          // the NodeSpec `type`
  /// Pure; the only place settings are checked; unknown keys are invalid.
  virtual std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings>
      parse(const NodeName& node, const NodeSettings& settings) const = 0;
};
}

// ===== node_catalogue.hpp =====
#pragma once
#include <memory>
#include <string_view>
#include <vector>
#include "ear/core/node_type.hpp"
namespace ear {
class NodeCatalogue {
public:
  /// Throws std::invalid_argument if two types share a name (boot-time check).
  explicit NodeCatalogue(std::vector<std::unique_ptr<const NodeType>> types);
  const NodeType* find(std::string_view name) const noexcept;   // nullptr: unknown
private:
  std::vector<std::unique_ptr<const NodeType>> types_;
};
}

// ===== desired_graph.hpp =====
#pragma once
#include <expected>
#include <memory>
#include "ear/core/node_catalogue.hpp"
namespace ear {
class DesiredNodeListener {
public:
  /// After the node is recorded; `config` lives as long as the node is wanted.
  virtual void nodeAdded(const NodeName& node, const NodeConfig& config) = 0;
protected:
  ~DesiredNodeListener() = default;
};
class DesiredRouteListener {
public:
  virtual void routeAdded(const Route& route) = 0;
protected:
  ~DesiredRouteListener() = default;
};
/// The control calls. M1: these two; the control port adds the rest later.
class GraphControl {
public:
  virtual ~GraphControl() = default;
  /// Same spec again: succeeds, no event. Errors: UnknownNodeType, InvalidSettings, DuplicateNode.
  virtual std::expected<void, CommandError> addNode(const NodeSpec& node) = 0;
  /// Existing route: succeeds, no event. Errors: UnknownNode, UnknownPort, WrongDirection.
  virtual std::expected<void, CommandError> addRoute(const Route& route) = 0;
};
class DesiredGraph final : public GraphControl {
public:
  explicit DesiredGraph(const NodeCatalogue& catalogue);   // catalogue outlives this
  ~DesiredGraph() override;
  /// Told in subscription order after the change is recorded; subscribe before the first call.
  void subscribe(DesiredNodeListener& listener);
  void subscribe(DesiredRouteListener& listener);
  std::expected<void, CommandError> addNode(const NodeSpec& node) override;
  std::expected<void, CommandError> addRoute(const Route& route) override;
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}

// ===== node_lifecycle.hpp =====
#pragma once
#include <optional>
#include <string_view>
#include "ear/core/node_type.hpp"
namespace ear {
enum class NodeState { starting, running, failed, stopping };   // "recovering" arrives with programs
std::string_view stateName(NodeState state) noexcept;
/// The one node state machine. Pure. Each fact returns true when the state changed; a fact
/// that does not apply changes nothing and returns false. No transitions after gone().
class NodeLifecycle {
public:
  NodeLifecycle() noexcept;                       // starting
  NodeState state() const noexcept;
  const std::optional<FailureReason>& reason() const noexcept;   // set only while failed
  bool gone() const noexcept;
  bool portBlockReady() noexcept;                 // starting -> running
  bool refused(FailureReason reason);             // starting|running -> failed
  bool stopRequested() noexcept;                  // starting|running|failed -> stopping
  bool stopped() noexcept;                        // stopping -> gone
private:
  NodeState state_; std::optional<FailureReason> reason_; bool gone_;
};
}

// ===== node_host.hpp =====
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include "ear/core/desired_graph.hpp"
#include "ear/core/node_lifecycle.hpp"
namespace ear {
struct NodeStatus { NodeName name; NodeState state; std::optional<FailureReason> reason; };
class NodeStatusListener {
public:
  virtual void nodeChanged(const NodeStatus& status) = 0;
  /// Everything the node made is gone. droppedFrames: frames its handoffs refused (ring full).
  virtual void nodeGone(const NodeName& node, std::uint64_t droppedFrames) = 0;
protected:
  ~NodeStatusListener() = default;
};
/// Engine side of a port block. Destroying it removes its ports and their links; after the
/// destructor returns, process() is never called again.
class PortBlock { public: virtual ~PortBlock() = default; };
class PortBlockEvents {           // main thread
public:
  virtual void ready() = 0;                    // processing in the engine; at most once
  virtual void lost(std::string detail) = 0;   // engine refused or dropped the block
protected:
  ~PortBlockEvents() = default;
};
class PortBlockFactory {
public:
  /// Main thread; code.prepare(format) already called. One engine node named `node` with
  /// exactly `ports` (32-bit float mono each), always processing, driven by `code` on the
  /// realtime thread. Refusal is reported via events.lost, never thrown. code/events outlive it.
  virtual std::unique_ptr<PortBlock> make(const NodeName& node, const Ports& ports,
      const AudioFormat& format, AudioCode& code, PortBlockEvents& events) = 0;
protected:
  ~PortBlockFactory() = default;
};
class Handoff {
public:
  virtual ~Handoff() = default;   // main thread; only after `closed` ran, or before any write
  virtual RtWriter& writer() noexcept = 0;
  /// Main thread, once, after the writing port block is gone: drains the rest to the consumer,
  /// calls consumer.finish() on the handoff thread, then `closed` on the main thread.
  virtual void close(std::move_only_function<void(std::uint64_t droppedFrames)> closed) = 0;
};
class HandoffFactory {
public:
  /// Main thread; starts the handoff's own ordinary thread.
  virtual std::unique_ptr<Handoff> make(const NodeName& node, std::uint32_t channels,
      std::uint32_t capacityFrames, HandoffConsumer& consumer) = 0;
protected:
  ~HandoffFactory() = default;
};
class NodeHost final : public DesiredNodeListener {
public:
  NodeHost(AudioFormat format, PortBlockFactory& blocks, HandoffFactory& handoffs);
  ~NodeHost();   // precondition: stopAll completed (or no node was ever added)
  void subscribe(NodeStatusListener& listener);
  /// starting -> config.start(grant) -> prepare -> port block; failures -> failed(reason).
  void nodeAdded(const NodeName& node, const NodeConfig& config) override;
  /// Every node: stopping; block destroyed, release(), handoffs closed, insides destroyed;
  /// gone. `allGone` runs once on the main thread after the last (at once if none).
  void stopAll(std::move_only_function<void()> allGone);
  std::vector<NodeStatus> status() const;
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}

// ===== reconciler.hpp =====
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "ear/core/desired_graph.hpp"
namespace ear {
using LinkId = std::uint64_t;          // issued by the Reconciler, unique per process
using EnginePortId = std::uint32_t;    // the engine's global id of a port
struct LivePort { PortRef port; Direction direction; EnginePortId id; };
class LivePortListener {
public:
  virtual void portAppeared(const LivePort& port) = 0;
  virtual void portGone(EnginePortId id) = 0;
protected:
  ~LivePortListener() = default;
};
class LinkListener {
public:
  virtual void linkActive(LinkId link) = 0;                      // audio flows
  virtual void linkFailed(LinkId link, std::string detail) = 0;  // refused; no link exists
  virtual void linkGone(LinkId link) = 0;                        // existed, now gone
protected:
  ~LinkListener() = default;
};
class LinkMaker {
public:
  /// Main thread. Exactly one of linkActive/linkFailed follows later, never inside this call.
  virtual void createLink(LinkId link, EnginePortId output, EnginePortId input) = 0;
  /// Main thread. Removes it if present; no further events for `link`.
  virtual void destroyLink(LinkId link) = 0;
protected:
  ~LinkMaker() = default;
};
enum class RouteMatch { pending, linked, refused };   // pending: an end is not live yet
std::string_view matchName(RouteMatch match) noexcept;
struct RouteStatus { Route route; RouteMatch match; std::string detail; };   // detail when refused
class RouteStatusListener {
public:
  virtual void routeChanged(const RouteStatus& status) = 0;
protected:
  ~RouteStatusListener() = default;
};
/// Invariants: at most one link per wanted route; links only for wanted routes; a route is
/// pending until both ends are live, then linked on linkActive, refused on linkFailed; an end
/// going (or linkGone) makes it pending and it is linked again when both ends are live.
class Reconciler final : public DesiredRouteListener, public LivePortListener, public LinkListener {
public:
  explicit Reconciler(LinkMaker& links);
  ~Reconciler();
  void subscribe(RouteStatusListener& listener);
  void routeAdded(const Route& route) override;
  void portAppeared(const LivePort& port) override;
  void portGone(EnginePortId id) override;
  void linkActive(LinkId link) override;
  void linkFailed(LinkId link, std::string detail) override;
  void linkGone(LinkId link) override;
  std::vector<RouteStatus> status() const;
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
```

```cpp
// ===== ear_nodes: src/nodes/include/ear/nodes/builtin.hpp =====
#pragma once
#include <memory>
#include <vector>
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "white": output "MONO", uniform noise in [-1, 1); settings {} (any key: invalid-settings).
std::unique_ptr<const NodeType> makeWhiteNodeType();
/// "file": inputs "FL", "FR"; settings {"path": non-empty string}. Truncates at start;
/// 32-bit float stereo WAV at the graph frequency; complete once the node is gone.
/// Unopenable path: start fails with startFailed. Ring: 2 s at the graph frequency.
std::unique_ptr<const NodeType> makeFileNodeType();
std::vector<std::unique_ptr<const NodeType>> builtinNodeTypes();   // white, file
}
```

```cpp
// ===== ear_pipewire: src/pipewire/include/ear/pipewire/engine.hpp =====
#pragma once
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
struct pw_loop; struct pw_context; struct pw_core;
namespace ear::pipewire {
struct EngineSettings {
  std::filesystem::path configFile;   // engine.conf; must not set default.clock.*
  std::uint32_t graphFrequency;       // -> default.clock.rate and the only allowed rate
  std::uint32_t blockSize;            // -> default.clock.quantum = min-quantum = max-quantum
};
struct EngineError { std::string detail; };
/// PipeWire's engine inside EAR: main loop, context, EAR's own internal connection. No socket
/// in M1 (core.daemon=false). Main thread only.
class Engine {
public:
  static std::expected<std::unique_ptr<Engine>, EngineError> create(const EngineSettings& settings);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  void run();                                      // runs the main loop until quit()
  void quit();                                     // from a main-loop callback
  void onStopSignal(std::function<void()> handler);   // SIGINT/SIGTERM, as main-loop events
  pw_loop* mainLoop() const noexcept;              // for this package's adapters only
  pw_context* context() const noexcept;
  pw_core* core() const noexcept;
private:
  struct Impl; std::unique_ptr<Impl> impl_;
  explicit Engine(std::unique_ptr<Impl> impl);
};
}

// ===== live_graph.hpp ===== includes: "ear/pipewire/engine.hpp", "ear/core/reconciler.hpp", <memory>
// registry -> LivePort events; joins port globals (node.id, port.name, port.direction) to node
// globals (node.name), in either arrival order.
namespace ear::pipewire {
class LiveGraph {
public:
  explicit LiveGraph(Engine& engine);
  ~LiveGraph();
  void subscribe(LivePortListener& listener);   // existing ports arrive later, never inside this call
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
// ===== links.hpp ===== includes: "ear/pipewire/engine.hpp", "ear/core/reconciler.hpp", <memory>
// link-factory by port global id, owned by EAR's connection (no linger); link proxy info state
// ACTIVE -> linkActive, ERROR/proxy error -> linkFailed, removed -> linkGone.
namespace ear::pipewire {
class Links final : public LinkMaker {
public:
  explicit Links(Engine& engine);
  ~Links();                                   // destroys every link it made
  void subscribe(LinkListener& listener);
  void createLink(LinkId link, EnginePortId output, EnginePortId input) override;
  void destroyLink(LinkId link) override;
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
// ===== port_blocks.hpp ===== includes: "ear/pipewire/engine.hpp", "ear/core/node_host.hpp", <memory>
// one pw_filter per node; one "32 bit float mono audio" DSP port per declared port;
// node.always-process=true; PW_FILTER_FLAG_RT_PROCESS. frames > maxBlock: outputs silenced,
// process() skipped. First STREAMING -> ready(); ERROR -> lost().
namespace ear::pipewire {
class FilterPortBlocks final : public PortBlockFactory {
public:
  explicit FilterPortBlocks(Engine& engine);
  std::unique_ptr<PortBlock> make(const NodeName& node, const Ports& ports,
      const AudioFormat& format, AudioCode& code, PortBlockEvents& events) override;
private:
  Engine& engine_;
};
}
// ===== handoffs.hpp ===== includes: "ear/core/node_host.hpp", <memory>; `struct pw_loop;` at global scope
// spa_ringbuffer + one pw_thread_loop per handoff, woken by pw_loop_signal_event.
namespace ear::pipewire {
class ThreadHandoffs final : public HandoffFactory {
public:
  explicit ThreadHandoffs(pw_loop* mainLoop);   // where `closed` runs; no Engine needed
  std::unique_ptr<Handoff> make(const NodeName& node, std::uint32_t channels,
      std::uint32_t capacityFrames, HandoffConsumer& consumer) override;
private:
  pw_loop* mainLoop_;
};
}
```

```cpp
// ===== ear_control: src/control/include/ear/control/{graph_file,status_log}.hpp =====
#pragma once
#include <expected>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include "ear/core/desired_graph.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/reconciler.hpp"
namespace ear::control {
struct GraphFileError { std::string detail; };   // names file and JSON pointer, e.g. "/routes/0/to/port: missing"
/// Pure. {"graphFrequency": uint>0, "blockSize": uint>0 (both required, no defaults),
///  "nodes": [{"name","type","settings"?}], "routes": [{"from":{"node","port"},"to":{...}}]}.
/// Unknown keys are errors. Node settings are passed through unchecked.
std::expected<GraphSpec, GraphFileError> parseGraphFile(std::string_view json);
std::expected<GraphSpec, GraphFileError> readGraphFile(const std::filesystem::path& path);
/// Boot: nodes in file order, then routes in file order; stops at the first error.
std::expected<void, CommandError> replay(const GraphSpec& spec, GraphControl& control);
/// M1's event stream (the control port replaces it). One flushed line per event:
///   node <n> starting|running|stopping
///   node <n> failed <failureCode>: <detail>
///   node <n> stopped dropped=<frames>
///   route <a>:<p> -> <b>:<q> pending|linked|refused[: <detail>]
class StatusLog final : public NodeStatusListener, public RouteStatusListener {
public:
  explicit StatusLog(std::ostream& out);
  void nodeChanged(const NodeStatus& status) override;
  void nodeGone(const NodeName& node, std::uint64_t droppedFrames) override;
  void routeChanged(const RouteStatus& status) override;
private:
  std::ostream& out_;
};
}
```

src/pipewire/engine.conf (S1 owns): context.properties core.daemon=false, core.name=ear, support.dbus=false,
NO default.clock.*; context.spa-libs support.* = support/libspa-support; context.modules:
module-rt [ifexists nofail], protocol-native, client-node [U needed for pw_filter export],
spa-node-factory, link-factory; context.objects: Dummy-Driver (support.node.driver,
priority.driver=200000). Copied next to out/ear; main passes <exe dir>/engine.conf; tests pass
the source-tree path; Engine does no lookup.

Boot in main (S6): parse CLI (--config, --help only) → readGraphFile → Engine::create → catalogue
from builtinNodeTypes(); DesiredGraph, NodeHost(FilterPortBlocks, ThreadHandoffs), LiveGraph,
Links, Reconciler, StatusLog; wire subscriptions → replay (error: print describe(), stopAll →
quit, exit 1) → onStopSignal → stopAll → quit → run → exit 0. Engine declared first (destroyed last).

## 3. Slice cut
Order: B (deps/manifests never fan out) → 0 (architect interfaces; may be written while B runs) →
lanes. Architect's order: S1 tracer then S2–S5 fan out, each in its own worktree lane. Main-loop
plan (page): three concurrent lanes after 0 — engine lane S1→S4, core lane S2→S3, nodes lane S5 —
then S6 join, then D. Departure: owner's e2e test lands in S6, not first; S1 proves the
frame-deciding risk through the real engine.
Slice 0 owns every CMakeLists.txt under src/*/ and test/system/ (test/system is added from src/CMakeLists.txt, after the ear target its tests spawn), creates each .cpp as a stub that
includes its header, placeholder tests. Each later slice owns only its .cpp/test files. New
file or link needed = erratum.
Land command: `git add -A -- <owned> && git commit -m '<msg>' -- <owned> && git pull --rebase origin claude/m1-white-to-file && git push origin HEAD:claude/m1-white-to-file`
Shared derived state: each worktree own out/; .cache/ccache shared; Boost tree shared read-only
via CMAKE_ARGS=-DFETCHCONTENT_SOURCE_DIR_BOOST=<main checkout>/.cache/deps/boost-src
(cmake/KeptFetchContent.cmake:30-35); Docker image shared read-only; scripts/dev.sh mounts
/Volumes/Dock/Home/Code/ear at the same path.

| Slice | Goal | Owned files | Size |
|---|---|---|---|
| B build base | one image dev+CI; JUCE gone; C++23; libpipewire + sndfile via pkg-config | CMakeLists.txt, src/CMakeLists.txt, Makefile (make test builds all + ctest; CMAKE_ARGS pass-through), docker/Dockerfile (base pinned by digest; libpipewire-0.3-dev libspa-0.2-dev libpipewire-0.3-modules libspa-0.2-modules libsndfile1-dev [U names], cmake, ccache, pkg-config, xz-utils), .dockerignore, scripts/dev.sh, .github/workflows/test.yml (one Linux job: buildx type=gha cache, run inside image; ccache + Boost via actions/cache; ctest logs on failure), src/main.cpp (stub program_options --help/--config), test/CMakeLists.txt, test/main.cpp (ear_catch_main lib; JUCE cleanup removed). Deletes include/ear/, src/server.hpp, test/audio_graph_test.cpp, test/virtual_audio_device.hpp, scripts/install-build-deps.sh. Boost trimmed to program_options. Subtree-bump vendor/json / vendor/catch ONLY if a C++23 probe fails | 4–6 h |
| 0 interfaces | §2 headers compile; package skeletons | every src/*/include/**, every src/*/CMakeLists.txt, stub .cpp, placeholder tests, test/system/ placeholders, first engine.conf | 3 h |
| S1 tracer: engine seam | real in-process PipeWire carries audio between two port blocks over a link | src/pipewire/{engine,live_graph,links,port_blocks}.cpp, src/pipewire/engine.conf, src/pipewire/tests/engine_test.cpp | 6–8 h |
| S2 node framework core | desired graph, catalogue, lifecycle (also defines failureCode, in node_lifecycle.cpp), NodeHost | src/core/{command_error,node_catalogue,desired_graph,node_lifecycle,node_host}.cpp, src/core/tests/{desired_graph,node_lifecycle,node_host}_test.cpp | 5–6 h |
| S3 reconciler | route links from events | src/core/reconciler.cpp, src/core/tests/reconciler_test.cpp | 2–3 h |
| S4 handoff | RT → ordinary-thread handoff | src/pipewire/handoffs.cpp, src/pipewire/tests/handoff_test.cpp | 3–4 h |
| S5 node types | white, file | src/nodes/{white,file,builtin}.cpp, src/nodes/tests/{white,file}_test.cpp | 3–4 h |
| S6 join | boot from ear.json, status lines, owner's test | src/control/{graph_file,status_log}.cpp, src/control/tests/graph_file_test.cpp, src/main.cpp, test/system/{white_to_file_test.cpp,child_process.hpp} | 4–5 h |
| D docs | AGENTS.md rewrite; remove juce-* skills; new .claude/skills/pipewire-engine/ from S1 findings; design record updated | AGENTS.md, .claude/skills/**, docs/** | 2–3 h |

### Acceptance criteria and mutation probes (all tests event-gated; a deadline only fails a test)
B: (1) scripts/dev.sh make test (macOS via Docker) and CI build image and ear; ctest ear_help
passes. Probe: --help returns 1. (2) configure fails naming the package when libpipewire-0.3 or
sndfile missing. Probe: drop libpipewire-0.3-dev from Dockerfile. (3) fresh configure fetches
Boost only. Probe: restore JUCE kept_fetchcontent → log shows JUCE fetch.
S1: (1) Engine::create succeeds with no daemon, no XDG_RUNTIME_DIR, no D-Bus; an unlinked sink
block reaches ready() and processes zeros. Probe: node.always-process=false → no ready() before
deadline. (2) LiveGraph reports both blocks' ports with node, port name, direction. Probe: invert
direction mapping. (3) output-only source block (ramp) linked by port ids: linkActive, then sink
sees contiguous ramp sample for sample [U]. Probe: adapter skips writing output spans.
(4) process sees clock.rate == graphFrequency and frames == blockSize, test uses 44100/256 [U].
Probe: stop passing clock properties. (5) destroying sink block → portGone and linkGone; process
count fixed after a later pw_core_sync done [U]. Probe: don't forward proxy removed. (6) SIGTERM
reaches onStopSignal. Probe: register SIGINT only.
S2: (1) addNode: UnknownNodeType; InvalidSettings; same spec twice one event; different settings
DuplicateNode. Probe: drop sameness check. (2) addRoute: UnknownNode, UnknownPort, WrongDirection;
duplicate route one event. Probe: drop direction check. (3) catalogue rejects duplicate type name.
Probe: remove check. (4) lifecycle full state × fact table. Probe: accept portBlockReady while
stopping. (5) NodeHost with fake factories: prepare before make; ready → running; start error →
failed start-failed; stopAll order block destroyed, release, handoffs closed, insides destroyed,
nodeGone(dropped), allGone once. Probe: destroy insides before close completes.
S3: (1) pending until both ends live; createLink exactly once with right ids. Probe: link when one
end live. (2) linkActive → linked; linkFailed → refused. Probe: map failed to pending. (3) an end
going → pending, relinked when it reappears. Probe: no relink. (4) ports of unwanted routes
ignored. Probe: link any matching pair.
S4: (1) frames from a writer thread arrive in order, interleaved, on the handoff thread (not writer
or main). Probe: swap channel order. (2) stalled consumer (latch) → short write return and
closed(dropped) == refused frames. Probe: return requested count. (3) close drains, finish once
after last consume, then closed on main loop. Probe: closed before finish. (4) write allocates
nothing (operator new counter on writer thread). Probe: allocate a vector in write.
S5: (1) white: ports {outputs: MONO}; any key invalid; 48000 frames all in [-1,1), RMS 0.577 ±
0.01. Probe: constant 0.5. (2) file via synchronous fake grant: 2-ch float WAV at format.rate with
exact samples FL then FR. Probe: swap interleave. (3) unopenable path → startFailed, no handoff
requested. Probe: ignore sf_open failure. (4) builtinNodeTypes() has white and file. Probe: drop one.
S6 (system tests spawn real out/ear): (1) white → file:FL: wait for `route noise:MONO -> rec:FL
linked`, 1 s window, SIGTERM, exit 0; WAV: over first..last nonzero FL sample RMS 0.577 ± 0.01,
span ≥ 0.5 s, no zero inside; FR exactly zero; rate = graphFrequency; float, 2 ch. Probe: replay
swaps port names. (2) routed to FR mirrors. Probe: hard-code FL. (3) clean stop prints `node rec
stopped dropped=0`, WAV header finished. Probe: quit without waiting for allGone. (4) route to
rec:FX → exit 1, output has `unknown-port rec:FX`. Probe: ignore addRoute errors.
(5) parseGraphFile units (missing graphFrequency names /graphFrequency). Probe: default to 48000.

## 4. Tests
Pure: ear_core_tests, ear_control_tests, ear_nodes_tests (temp files) — ms, no PipeWire symbol.
In-process PipeWire: ear_pipewire_tests ~1 s each, no daemon/hardware. System: ear_system_tests 3
cases ~1.5 s, parallel under ctest. One pinned image locally (scripts/dev.sh; Docker storage on
/Volumes/Dock) and in CI. Build without JUCE est. 1–2 min cold (not measured).

## 5. JUCE
Removed in B (owner's answer to Q2, 2026-10-29): JUCE, ear_tests, virtual device, install-build-deps.sh, macOS CI
leg. ear.json v1 and --device/--gain flags dropped without compat. Departs from outline step 9.

## 6. Risks
Folded into S1: engine + dummy driver; output-only source; always-process without SM; clock props;
no process after pw_filter_destroy; client-node enough. B: C++23 with old vendored libs; package
names. Unverified: 1.4.2 behaviour matching 1.0.5 scheduling; pw_loop_signal_event RT-safe.
After the work: S1's tests passed on 1.4.2 for every item folded into S1 (so 1.4.2 schedules
unlinked always-process nodes as 1.0.5 does); S4 showed the handoff write, wake included,
allocates nothing, but RT safety beyond that is still unverified.

## 7. Owner questions (on the gate page)
Q1 container base: Debian 13 (rec; PipeWire 1.4.2, GCC 14) vs Ubuntu 24.04 (PipeWire 1.0.5, GCC 13).
Answered: Debian 13.
Q2 remove JUCE now (rec) vs keep until sound-card output. Answered: remove now.

## 8. Parked
Control port 7777 + rest of GraphControl; Beast + PipeWire one main loop. Socket (core.daemon,
runtime dir); real ownership mark (client id / security context); name-taken. Program supervisor,
airplay, recovering + backoff. Engine-node and module port-block kinds (NodeInsides::audioCode()
generalises). Live rate/block changes (prepare again); multiple links mixing. media.class; pw-top
attach. Handoff batching / shared threads. Production image, k8s, RT privileges, mlock; ALSA + R5.
PipeWire ≥1.7: load module-scheduler-v1. Reload/persist ear.json. Status/drop counts over port.
Maintenance risks: engine.conf tied to pinned PipeWire (bump procedure in .claude/skills/pipewire-engine/SKILL.md); node.name
recognition only while no other client; one thread per handoff; stuck disk I/O hangs SIGTERM;
lanes need the Boost override or refetch 800 MB.

## New terms
Handoff (ring + wake + ordinary thread), Live port, Graph file (ear.json v2), Status line, Lane.
NOT RUN: nothing was built or compiled by the architect.

## Errata (corrected after the work, slice D)
- Land command named branch claude/pipewire-design; the work landed on claude/m1-white-to-file.
- engine.conf's path was not given; it is src/pipewire/engine.conf (configure copies it beside out/ear).
- test/system/ is added from src/CMakeLists.txt, not the top-level CMakeLists.txt (slice 0's choice): its tests need the ear target.
- failureCode (node_type.hpp) had no owning slice; S2 defines it, in src/core/node_lifecycle.cpp.
- Results: every slice landed; ctest in the Debian image passed 60 of 60 tests, and CI passed. S6 checks criterion 1's "no zero inside" as "no two adjacent zero samples" (white can produce an exact 0 with chance 2^-24 per sample).
