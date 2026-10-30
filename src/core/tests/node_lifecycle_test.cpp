// The node state machine, every state against every fact (docs/design/m1-white-to-file.md
// section 3, S2 criterion 4).
#include <functional>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/node_lifecycle.hpp"

using namespace ear;

namespace {

const FailureReason refusal{FailureReason::Code::engineRefused, "no driver"};

/// The five states a node can be in ("gone" is stopping plus gone()), each reached by facts.
enum class At { starting, running, failed, stopping, gone };

NodeLifecycle reach(At at) {
	NodeLifecycle node;
	switch (at) {
	case At::starting: break;
	case At::running: REQUIRE(node.portBlockReady()); break;
	case At::failed: REQUIRE(node.refused(refusal)); break;
	case At::stopping: REQUIRE(node.stopRequested()); break;
	case At::gone:
		REQUIRE(node.stopRequested());
		REQUIRE(node.stopped());
		break;
	}
	return node;
}

At where(const NodeLifecycle& node) {
	if (node.gone()) return At::gone;
	switch (node.state()) {
	case NodeState::starting: return At::starting;
	case NodeState::running: return At::running;
	case NodeState::failed: return At::failed;
	case NodeState::stopping: return At::stopping;
	}
	FAIL("unknown state");
	return At::gone;
}

struct Fact {
	std::string name;
	std::function<bool(NodeLifecycle&)> apply;
};

const std::vector<Fact> facts{
	{"portBlockReady", [](NodeLifecycle& n) { return n.portBlockReady(); }},
	{"refused", [](NodeLifecycle& n) { return n.refused(refusal); }},
	{"stopRequested", [](NodeLifecycle& n) { return n.stopRequested(); }},
	{"stopped", [](NodeLifecycle& n) { return n.stopped(); }},
};

} // namespace

TEST_CASE("A new node is starting with no reason", "[lifecycle]") {
	const NodeLifecycle node;
	CHECK(node.state() == NodeState::starting);
	CHECK_FALSE(node.reason().has_value());
	CHECK_FALSE(node.gone());
}

TEST_CASE("Every fact in every state moves the node as the table says", "[lifecycle]") {
	// Rows: states. Columns: portBlockReady, refused, stopRequested, stopped. Each cell: where
	// the node is afterwards; the fact reports a change exactly when the cell differs from the row.
	const std::vector<std::pair<At, std::vector<At>>> table{
		{At::starting, {At::running, At::failed, At::stopping, At::starting}},
		{At::running, {At::running, At::failed, At::stopping, At::running}},
		{At::failed, {At::failed, At::failed, At::stopping, At::failed}},
		{At::stopping, {At::stopping, At::stopping, At::stopping, At::gone}},
		{At::gone, {At::gone, At::gone, At::gone, At::gone}},
	};
	for (const auto& [from, after] : table) {
		for (std::size_t i = 0; i < facts.size(); ++i) {
			INFO("state " << static_cast<int>(from) << ", fact " << facts[i].name);
			NodeLifecycle node = reach(from);
			const bool changed = facts[i].apply(node);
			CHECK(where(node) == after[i]);
			CHECK(changed == (after[i] != from));
			// A reason is set exactly while failed.
			CHECK(node.reason().has_value() == (where(node) == At::failed));
		}
	}
}

TEST_CASE("A refused node keeps its reason until it is asked to stop", "[lifecycle]") {
	NodeLifecycle node;
	REQUIRE(node.portBlockReady());
	REQUIRE(node.refused({FailureReason::Code::startFailed, "cannot open /nope"}));
	REQUIRE(node.reason().has_value());
	CHECK(node.reason()->code == FailureReason::Code::startFailed);
	CHECK(node.reason()->detail == "cannot open /nope");
	CHECK_FALSE(node.refused(refusal));
	CHECK(node.reason()->code == FailureReason::Code::startFailed);
}

TEST_CASE("States and failure codes have their status-line names", "[lifecycle]") {
	CHECK(stateName(NodeState::starting) == "starting");
	CHECK(stateName(NodeState::running) == "running");
	CHECK(stateName(NodeState::failed) == "failed");
	CHECK(stateName(NodeState::stopping) == "stopping");
	CHECK(failureCode(FailureReason::Code::engineRefused) == "engine-refused");
	CHECK(failureCode(FailureReason::Code::startFailed) == "start-failed");
}
