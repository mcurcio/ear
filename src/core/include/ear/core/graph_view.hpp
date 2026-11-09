// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit. NodeView's health and since are
// docs/design/node-lifecycle-modules.md 5.2's (slice 3a, step L-6; batch2.md section 11's erratum).
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
/// A wanted node as a snapshot shows it (docs/design/node-lifecycle-modules.md 5.2, step L-6): its spec (`disabled`
/// among it), its public ports, its health, and when its phase began, which the gateway fills in (D-M11) and the read
/// leaves empty.
struct NodeView {
	NodeSpec spec;
	Ports ports; // public only (inside lists empty)
	NodeHealth health;
	std::optional<std::chrono::system_clock::time_point> since{};
};
/// A wanted node's health before NodeHost told any (D-M12): added and wanted running, or disabled from the spec.
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
