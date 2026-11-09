// What must not compile about ear's latency model (ear/core/latency.hpp; src/core/CMakeLists.txt registers one CTest
// test per case, each with the reason it must be refused). With no case defined this is the control, and it must
// compile: what the cases break is only what they name.
#include <optional>

#include "ear/core/latency.hpp"

namespace latency_bad {

using namespace ear;

void control() {
	Paths paths;
	paths.add(Latency{0, 0});
	paths.add(std::optional<Latency>{});
	const Latency told = paths.orZero() + Delay{Latency{480, 480}};
	static_cast<void>(told);
}

#ifdef EAR_CASE_beyond_as_peer
// A companion's buffer beyond a bank lies in series with the bank's peers; combined as one more peer, peers reporting
// zero would make its least zero (the 3b.3 regression).
void bad() {
	latency_model::LatencyPlan plan{.beyond = {{"from_rtp", Delay{Latency{480, 480}}}}};
	Paths peers;
	peers.add(plan.beyond.front().second);
}
#endif

#ifdef EAR_CASE_delay_as_path
void bad() {
	Paths paths;
	paths.add(Delay{Latency{0, 64}});
}
#endif

#ifdef EAR_CASE_delay_as_latency
// A delay never passes for a path: it converts to nothing.
void bad() {
	const Latency path = Delay{Latency{0, 64}};
	static_cast<void>(path);
}
#endif

#ifdef EAR_CASE_delay_as_optional_peer
void bad() {
	Paths paths;
	paths.add(std::optional<Latency>(Delay{Latency{0, 64}}));
}
#endif

#ifdef EAR_CASE_paths_in_series
// Parallel paths are combined first, then added in series: Paths itself has no +.
void bad() {
	Paths paths;
	const Latency sum = Latency{1, 1} + paths;
	static_cast<void>(sum);
}
#endif

} // namespace latency_bad
