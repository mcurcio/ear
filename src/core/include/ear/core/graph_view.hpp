#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <vector>
#include "ear/core/desired_graph.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/reconciler.hpp"
namespace ear {
/// Engine facts fixed at boot.
struct EngineView {
	AudioFormat format;     // rate = graphFrequency, maxBlock = blockSize
	bool realtime;          // the data thread got realtime priority
	std::string scheduling; // e.g. "SCHED_FIFO 88", "SCHED_OTHER"
};
/// A wanted node as a snapshot shows it (docs/control-port.md#node-health): its spec (`disabled`
/// among it), its public ports, its health, and when its phase began, which the gateway fills in and the read
/// leaves empty.
struct NodeView {
	NodeSpec spec;
	Ports ports; // public only (inside lists empty)
	NodeHealth health;
	std::optional<std::chrono::system_clock::time_point> since{};
};
/// A wanted node's health before NodeHost told any: added and wanted running, or disabled from the spec.
NodeHealth healthFromSpec(const NodeSpec& spec);
struct GraphView {
	EngineView engine;
	std::vector<std::string> types;  // sorted
	std::vector<NodeView> nodes;     // wanted nodes, name order
	std::vector<RouteStatus> routes; // wanted routes, Route order
};
class GraphReader {
public:
	virtual GraphView read() const = 0; // main thread
protected:
	~GraphReader() = default;
};
/// The join of the desired graph, the node framework and the reconciler. A wanted node's health
/// is NodeHost's last entry of its name, or healthFromSpec when NodeHost has none yet.
class GraphStatus final : public GraphReader {
public:
	GraphStatus(EngineView engine, const NodeCatalogue& catalogue, const DesiredGraph& desired, const NodeHost& nodes,
	            const Reconciler& routes); // all outlive it
	GraphView read() const override;

private:
	EngineView engine_;
	const NodeCatalogue& catalogue_;
	const DesiredGraph& desired_;
	const NodeHost& nodes_;
	const Reconciler& routes_;
};
} // namespace ear
