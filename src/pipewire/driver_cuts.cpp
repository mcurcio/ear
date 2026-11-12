// The engine's record of the internal clock's cuts (driver_cuts.hpp, docs/engine.md#graphcycle).
#include "driver_cuts.hpp"

#include <string>
#include <string_view>

#include <pipewire/impl.h>

#include "engine_names.hpp"
#include "engine_properties.hpp"

namespace ear::pipewire {
namespace {
/// The name the record looks for is the one engine.conf gives the internal clock, by the property table's row.
constexpr bool tableNamesTheClock() {
	for (const PropertyRow& row : engineProperties)
		if (row.home == Home::internalClock && row.key == "node.name") return row.value == internalClockNode;
	return false;
}
static_assert(tableNamesTheClock(), "the internal clock's node.name in the property table is not internalClockNode");
} // namespace

std::expected<std::unique_ptr<DriverCuts>, EngineError> DriverCuts::listen(pw_context* context) {
	pw_impl_node* clock = engineNode(context, internalClockNode);
	if (clock == nullptr)
		return std::unexpected(EngineError{std::string("the engine holds no internal clock (") + internalClockNode +
		                                   "): engine.conf's context.objects must make it"});
	const pw_node_info* info = pw_impl_node_get_info(clock);
	return std::unique_ptr<DriverCuts>(new DriverCuts(clock, info->id));
}

DriverCuts::DriverCuts(pw_impl_node* clock, std::uint32_t clockId) : clockId_(clockId) {
	// Added by an invoke posted on the data loop before any block exists, and every block's node reaches the driver by
	// invokes posted after it, which that loop runs in posting order: the listener hears every block's first cycle.
	listenRealtime<NodeRealtimeCallbacks<CutRing>{
		.start = realtime<&CutRing::start>, .incomplete = realtime<&CutRing::incomplete>}>(clock, listener_,
	                                                                                       std::make_unique<CutRing>());
}

} // namespace ear::pipewire
