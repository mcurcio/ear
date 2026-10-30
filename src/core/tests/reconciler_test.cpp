// The reconciler driven by events through a fake LinkMaker: when it asks for a link, which
// ports it joins, and the status each route reaches from the link events
// (docs/design/m1-white-to-file.md section 3, S3 criteria 1-4). Every engine reply is an event
// the test sends.
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/reconciler.hpp"

using namespace ear;

namespace {

struct FakeLinks final : LinkMaker {
	struct Made { LinkId link; EnginePortId output; EnginePortId input; };
	std::vector<Made> made;
	std::vector<LinkId> destroyed;
	void createLink(LinkId link, EnginePortId output, EnginePortId input) override {
		made.push_back({link, output, input});
	}
	void destroyLink(LinkId link) override { destroyed.push_back(link); }
};

struct StatusLog final : RouteStatusListener {
	std::vector<RouteStatus> changes;
	void routeChanged(const RouteStatus& status) override { changes.push_back(status); }
};

const Route noiseToFl{{"noise", "MONO"}, {"rec", "FL"}};
const LivePort noiseOut{{"noise", "MONO"}, Direction::output, 10};
const LivePort recFl{{"rec", "FL"}, Direction::input, 20};
const LivePort recFr{{"rec", "FR"}, Direction::input, 21};

struct Fixture {
	FakeLinks links;
	StatusLog log;
	Reconciler reconciler{links};
	Fixture() { reconciler.subscribe(log); }

	RouteMatch match() const {
		const auto all = reconciler.status();
		REQUIRE(all.size() == 1);
		return all.front().match;
	}
};

} // namespace

TEST_CASE("A route stays pending until both ends are live, then one link joins them", "[reconciler]") {
	Fixture f;
	f.reconciler.routeAdded(noiseToFl);
	REQUIRE(f.log.changes.size() == 1);
	CHECK(f.log.changes.front().route == noiseToFl);
	CHECK(f.log.changes.front().match == RouteMatch::pending);

	f.reconciler.portAppeared(noiseOut);
	CHECK(f.links.made.empty());
	CHECK(f.match() == RouteMatch::pending);

	f.reconciler.portAppeared(recFr);   // the other input port of the same node
	CHECK(f.links.made.empty());

	f.reconciler.portAppeared(recFl);
	REQUIRE(f.links.made.size() == 1);
	CHECK(f.links.made.front().output == noiseOut.id);
	CHECK(f.links.made.front().input == recFl.id);
	CHECK(f.match() == RouteMatch::pending);   // asked for, not yet active

	f.reconciler.routeAdded(noiseToFl);   // the same route again
	f.reconciler.portAppeared(recFr);     // and an unrelated port again
	CHECK(f.links.made.size() == 1);
	CHECK(f.log.changes.size() == 1);
}

TEST_CASE("A route whose ports are live before it is wanted links when it is added", "[reconciler]") {
	Fixture f;
	f.reconciler.portAppeared(recFl);
	f.reconciler.portAppeared(noiseOut);
	CHECK(f.links.made.empty());
	f.reconciler.routeAdded(noiseToFl);
	REQUIRE(f.links.made.size() == 1);
	CHECK(f.links.made.front().output == noiseOut.id);
	CHECK(f.links.made.front().input == recFl.id);
}

TEST_CASE("linkActive makes a route linked and linkFailed makes it refused with the detail", "[reconciler]") {
	Fixture f;
	f.reconciler.routeAdded(noiseToFl);
	f.reconciler.portAppeared(noiseOut);
	f.reconciler.portAppeared(recFl);
	REQUIRE(f.links.made.size() == 1);
	const LinkId link = f.links.made.front().link;

	SECTION("active") {
		f.reconciler.linkActive(link);
		CHECK(f.match() == RouteMatch::linked);
		REQUIRE(f.log.changes.size() == 2);
		CHECK(f.log.changes.back().match == RouteMatch::linked);
		CHECK(matchName(f.log.changes.back().match) == "linked");
	}
	SECTION("failed") {
		f.reconciler.linkFailed(link, "format mismatch");
		const auto all = f.reconciler.status();
		REQUIRE(all.size() == 1);
		CHECK(all.front().match == RouteMatch::refused);
		CHECK(all.front().detail == "format mismatch");
		REQUIRE(f.log.changes.size() == 2);
		CHECK(f.log.changes.back().match == RouteMatch::refused);
		CHECK(f.log.changes.back().detail == "format mismatch");
		CHECK(matchName(RouteMatch::refused) == "refused");
		CHECK(f.links.made.size() == 1);   // no retry while the same ends stand
	}
	SECTION("events for a link it never asked for change nothing") {
		f.reconciler.linkActive(link + 100);
		f.reconciler.linkFailed(link + 100, "x");
		f.reconciler.linkGone(link + 100);
		CHECK(f.match() == RouteMatch::pending);
		CHECK(f.log.changes.size() == 1);
	}
}

TEST_CASE("An end going makes a route pending, and it is linked again when the end reappears", "[reconciler]") {
	Fixture f;
	f.reconciler.routeAdded(noiseToFl);
	f.reconciler.portAppeared(noiseOut);
	f.reconciler.portAppeared(recFl);
	REQUIRE(f.links.made.size() == 1);
	const LinkId first = f.links.made.front().link;
	f.reconciler.linkActive(first);
	REQUIRE(f.match() == RouteMatch::linked);

	f.reconciler.portGone(recFl.id);
	CHECK(f.match() == RouteMatch::pending);
	CHECK(f.links.destroyed == std::vector<LinkId>{first});
	CHECK(f.links.made.size() == 1);

	const LivePort recFlAgain{recFl.port, Direction::input, 30};
	f.reconciler.portAppeared(recFlAgain);
	REQUIRE(f.links.made.size() == 2);
	const auto second = f.links.made.back();
	CHECK(second.link != first);
	CHECK(second.output == noiseOut.id);
	CHECK(second.input == recFlAgain.id);

	f.reconciler.linkActive(first);   // a stale event for the destroyed link
	CHECK(f.match() == RouteMatch::pending);
	f.reconciler.linkActive(second.link);
	CHECK(f.match() == RouteMatch::linked);

	SECTION("linkGone makes it pending and links again while both ends are live") {
		f.reconciler.linkGone(second.link);
		CHECK(f.match() == RouteMatch::pending);
		REQUIRE(f.links.made.size() == 3);
		CHECK(f.links.made.back().output == noiseOut.id);
		CHECK(f.links.made.back().input == recFlAgain.id);
	}
	SECTION("a refused route is tried again after an end goes and comes back") {
		f.reconciler.portGone(noiseOut.id);
		REQUIRE(f.links.made.size() == 2);
		f.reconciler.portAppeared(noiseOut);
		REQUIRE(f.links.made.size() == 3);
		f.reconciler.linkFailed(f.links.made.back().link, "no");
		REQUIRE(f.match() == RouteMatch::refused);
		f.reconciler.portGone(noiseOut.id);
		CHECK(f.match() == RouteMatch::pending);
		CHECK(f.links.destroyed.size() == 2);   // the refused attempt left no link to destroy
		f.reconciler.portAppeared(noiseOut);
		CHECK(f.links.made.size() == 4);
	}
}

TEST_CASE("Ports of routes nobody wants are never linked", "[reconciler]") {
	Fixture f;
	f.reconciler.routeAdded(noiseToFl);
	f.reconciler.portAppeared(noiseOut);
	f.reconciler.portAppeared(recFr);
	f.reconciler.portAppeared({{"other", "MONO"}, Direction::output, 40});
	f.reconciler.portAppeared({{"rec", "FL"}, Direction::output, 41});   // right name, wrong direction
	CHECK(f.links.made.empty());
	CHECK(f.match() == RouteMatch::pending);

	f.reconciler.portAppeared(recFl);
	REQUIRE(f.links.made.size() == 1);
	CHECK(f.links.made.front().output == noiseOut.id);
	CHECK(f.links.made.front().input == recFl.id);
}
