// The graph spec's value types compare by value: the desired graph's "same spec again" and
// "existing route" rules and the reconciler's route lookups rest on it.
#include <set>

#include "catch2/catch.hpp"
#include "ear/core/graph_spec.hpp"

using ear::NodeSpec;
using ear::PortRef;
using ear::Route;

TEST_CASE("A node spec equals another only with the same name, type and settings", "[graph_spec]") {
	const NodeSpec rec{"rec", "file", {{"path", "a.wav"}}};

	CHECK(rec == NodeSpec{"rec", "file", {{"path", "a.wav"}}});
	CHECK(rec != NodeSpec{"rec", "file", {{"path", "b.wav"}}});
	CHECK(rec != NodeSpec{"rec", "white", {{"path", "a.wav"}}});
	CHECK(rec != NodeSpec{"tap", "file", {{"path", "a.wav"}}});
}

TEST_CASE("Routes compare by both ends, port by port", "[graph_spec]") {
	const Route toLeft{{"noise", "noise.mono"}, {"rec", "record.left"}};
	const Route toRight{{"noise", "noise.mono"}, {"rec", "record.right"}};

	CHECK(toLeft == Route{PortRef{"noise", "noise.mono"}, PortRef{"rec", "record.left"}});
	CHECK(toLeft != toRight);
	CHECK(toLeft != Route{{"rec", "record.left"}, {"noise", "noise.mono"}});

	const std::set<Route> routes{toLeft, toRight, toLeft};
	CHECK(routes.size() == 2);
	CHECK(routes.contains(Route{{"noise", "noise.mono"}, {"rec", "record.right"}}));
}
