// The graph read: the join of the desired graph, the node framework and the reconciler
// (docs/design/batch2.md section 4.2, graph_view.hpp). Slice s0 stub: slice C1 builds read().
#include "ear/core/graph_view.hpp"

#include <stdexcept>
#include <utility>

namespace ear {

GraphStatus::GraphStatus(EngineView engine, const NodeCatalogue& catalogue, const DesiredGraph& desired,
                         const NodeHost& nodes, const Reconciler& routes)
	: engine_(std::move(engine)), catalogue_(catalogue), desired_(desired), nodes_(nodes), routes_(routes) {}

GraphView GraphStatus::read() const {
	throw std::logic_error("GraphStatus::read: not built yet (slice C1)");
}

} // namespace ear
