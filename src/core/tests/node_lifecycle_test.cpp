// The node state machine, every state against every fact (docs/design/batch2.md section 4.1,
// node_lifecycle.hpp; C1 criterion 12): running needs the port block and the insides ready, and
// a lost port block is final.
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/node_lifecycle.hpp"

using namespace ear;

namespace {

const FailureReason insidesFailure{FailureReason::Code::startFailed, "cannot open /nope"};
const FailureReason blockLoss{FailureReason::Code::engineRefused, "no driver"};

/// Where a node can be. `lost` is failed with its port block lost; `blockReady` is starting with
/// only the port block ready; `gone` is stopping plus gone().
enum class At { starting, blockReady, running, failed, lost, stopping, gone };

NodeLifecycle reach(At at) {
	NodeLifecycle node;
	switch (at) {
	case At::starting: break;
	case At::blockReady: REQUIRE_FALSE(node.portBlockReady()); break;
	case At::running:
		REQUIRE_FALSE(node.portBlockReady());
		REQUIRE(node.insidesReady());
		break;
	case At::failed: REQUIRE(node.insidesFailed(insidesFailure)); break;
	case At::lost: REQUIRE(node.portBlockLost(blockLoss)); break;
	case At::stopping: REQUIRE(node.stopRequested()); break;
	case At::gone:
		REQUIRE(node.stopRequested());
		REQUIRE(node.stopped());
		break;
	}
	return node;
}

/// The state alone cannot tell starting from blockReady: a copy told its insides are ready
/// runs only if its block is ready.
At where(const NodeLifecycle& node) {
	if (node.gone()) return At::gone;
	switch (node.state()) {
	case NodeState::starting: {
		NodeLifecycle copy = node;
		copy.insidesReady();
		return copy.state() == NodeState::running ? At::blockReady : At::starting;
	}
	case NodeState::running: return At::running;
	case NodeState::failed: return node.blockLost() ? At::lost : At::failed;
	case NodeState::stopping: return At::stopping;
	}
	FAIL("unknown state");
	return At::gone;
}

/// The reason a node at `at` holds, by where it is: the block's reason once lost, else the
/// insides' reason (the only other failure the table reaches).
std::optional<FailureReason::Code> reasonAt(At at) {
	if (at == At::lost) return blockLoss.code;
	if (at == At::failed) return insidesFailure.code;
	return std::nullopt;
}

struct Fact {
	std::string name;
	std::function<bool(NodeLifecycle&)> apply;
};

const std::vector<Fact> facts{
	{"portBlockReady", [](NodeLifecycle& n) { return n.portBlockReady(); }},
	{"portBlockLost", [](NodeLifecycle& n) { return n.portBlockLost(blockLoss); }},
	{"insidesReady", [](NodeLifecycle& n) { return n.insidesReady(); }},
	{"insidesFailed", [](NodeLifecycle& n) { return n.insidesFailed(insidesFailure); }},
	{"restartRequested", [](NodeLifecycle& n) { return n.restartRequested(); }},
	{"stopRequested", [](NodeLifecycle& n) { return n.stopRequested(); }},
	{"stopped", [](NodeLifecycle& n) { return n.stopped(); }},
};

const char* atName(At at) {
	switch (at) {
	case At::starting: return "starting";
	case At::blockReady: return "starting, block ready";
	case At::running: return "running";
	case At::failed: return "failed";
	case At::lost: return "failed, block lost";
	case At::stopping: return "stopping";
	case At::gone: return "gone";
	}
	return "?";
}

} // namespace

TEST_CASE("A new node is starting with no reason", "[lifecycle]") {
	const NodeLifecycle node;
	CHECK(node.state() == NodeState::starting);
	CHECK_FALSE(node.reason().has_value());
	CHECK_FALSE(node.gone());
	CHECK_FALSE(node.blockLost());
}

TEST_CASE("Every fact in every state moves the node as the table says", "[lifecycle]") {
	// Columns: portBlockReady, portBlockLost, insidesReady, insidesFailed, restartRequested,
	// stopRequested, stopped. Each cell: where the node is afterwards; the fact reports a change
	// exactly when the cell differs from the row (the block becoming ready while starting is not a
	// change of state). A lost block is final: only stopping leaves it.
	using enum At;
	const std::vector<std::pair<At, std::vector<At>>> table{
		{starting, {blockReady, lost, starting, failed, starting, stopping, starting}},
		{blockReady, {blockReady, lost, running, failed, blockReady, stopping, blockReady}},
		{running, {running, lost, running, failed, blockReady, stopping, running}},
		{failed, {failed, lost, failed, failed, starting, stopping, failed}},
		{lost, {lost, lost, lost, lost, lost, stopping, lost}},
		{stopping, {stopping, stopping, stopping, stopping, stopping, stopping, gone}},
		{gone, {gone, gone, gone, gone, gone, gone, gone}},
	};
	auto sameState = [](At a, At b) {
		const auto norm = [](At x) { return x == blockReady ? starting : x; };
		return norm(a) == norm(b);
	};
	for (const auto& [from, after] : table) {
		for (std::size_t i = 0; i < facts.size(); ++i) {
			INFO("state " << atName(from) << ", fact " << facts[i].name);
			NodeLifecycle node = reach(from);
			const bool changed = facts[i].apply(node);
			CHECK(atName(where(node)) == std::string(atName(after[i])));
			CHECK(changed == !sameState(after[i], from));
			// A reason is set exactly while failed, and it is the block's once the block is lost.
			const auto code = node.reason() ? std::optional{node.reason()->code} : std::nullopt;
			CHECK(code == reasonAt(after[i]));
		}
	}
}

TEST_CASE("A node runs once its port block and its insides are both ready, in either order",
          "[lifecycle]") {
	NodeLifecycle blockFirst;
	CHECK_FALSE(blockFirst.portBlockReady());
	CHECK(blockFirst.state() == NodeState::starting);
	CHECK(blockFirst.insidesReady());
	CHECK(blockFirst.state() == NodeState::running);

	NodeLifecycle insidesFirst;
	CHECK_FALSE(insidesFirst.insidesReady());
	CHECK(insidesFirst.state() == NodeState::starting);
	CHECK(insidesFirst.portBlockReady());
	CHECK(insidesFirst.state() == NodeState::running);
}

TEST_CASE("A restart keeps a ready block: new insides alone make the node running again", "[lifecycle]") {
	NodeLifecycle node = reach(At::running);
	REQUIRE(node.restartRequested());
	CHECK(node.state() == NodeState::starting);
	CHECK(node.insidesReady());
	CHECK(node.state() == NodeState::running);
}

TEST_CASE("A lost block's reason replaces an insides reason and stays through every later fact",
          "[lifecycle]") {
	NodeLifecycle node;
	REQUIRE(node.insidesFailed(insidesFailure));
	REQUIRE(node.portBlockLost(blockLoss));
	for (const auto& fact : facts) {
		if (fact.name == "stopRequested" || fact.name == "stopped") continue;
		INFO(fact.name);
		CHECK_FALSE(fact.apply(node));
		CHECK(node.state() == NodeState::failed);
		REQUIRE(node.reason().has_value());
		CHECK(node.reason()->detail == "no driver");
	}
	CHECK(node.portBlockLost({FailureReason::Code::engineRefused, "another detail"}));
	CHECK(node.reason()->detail == "another detail");
}

TEST_CASE("A failed node keeps its reason until it is asked to stop", "[lifecycle]") {
	NodeLifecycle node;
	REQUIRE_FALSE(node.portBlockReady());
	REQUIRE(node.insidesFailed({FailureReason::Code::startFailed, "cannot open /nope"}));
	REQUIRE(node.reason().has_value());
	CHECK(node.reason()->code == FailureReason::Code::startFailed);
	CHECK(node.reason()->detail == "cannot open /nope");
	CHECK_FALSE(node.insidesFailed(blockLoss));
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
