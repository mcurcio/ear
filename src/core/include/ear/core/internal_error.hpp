// ear's own bug, caught where its owner is known (docs/architecture.md#faults-by-owner): what the operator is told
// (docs/output-records.md#diagnostic, ear.internal-error) and the one wording of the failure it gives a node.
#pragma once
#include <string>
#include <string_view>
namespace ear {
/// One caught bug of ear's own: the callback or adapter call that threw ("FilterPortBlock::onStateChanged",
/// "SinkAdapter::make"); the node it fails, or empty when it fails none (a route's link) or ends the process; where the
/// main loop was when it was thrown (MainLoopActivity's "<call> <node>", or empty); what it said; and whether ear ends
/// right after it (no owner but ear's core). Every view lives only for the call it is passed to.
struct InternalError {
	std::string_view callback;
	std::string_view node;
	std::string_view where;
	std::string_view what;
	bool fatal = false;
};
/// Where a caught bug is told (standard output's encoder). Called on the thread that caught it, which may not be the
/// main loop's: the callback boundary reports on whichever thread its callback ran.
class InternalErrorListener {
public:
	virtual void internalError(const InternalError& error) = 0;

protected:
	~InternalErrorListener() = default;
};
/// The failure a node is given for a bug of one of its adapters: "internal error in <callback>: <what>". The one
/// wording, for NodeHost and the engine's adapters both.
inline std::string internalErrorDetail(std::string_view callback, std::string_view what) {
	std::string detail = "internal error in ";
	detail += callback;
	detail += ": ";
	detail += what;
	return detail;
}
} // namespace ear
