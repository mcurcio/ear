// A change of a node's ports with its type kept (docs/architecture.md#reshapes), by a test type in the type harness:
// in place when every changed key is, the old code keeping its channels playing until the commit's swap brings the code
// for the new ones; a restart with the block reshaped between the configurations when a changed key is restart-only; a
// change version that keeps the old code fails the node.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/testing/type_harness.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace reshape_test {
using namespace ear;

constexpr auto channelsRow = setting::defaulted("channels", setting::number(1, 4), 2).inPlace();
constexpr auto modeRow = setting::optional("mode", setting::number(0, 9)); // restart-only
constexpr auto out = bank::output<"out">;

constexpr TypeSpec spec{.settings = SettingsTable<channelsRow, modeRow>{"shaper"}, .banks = Banks<out>{}};

/// What the type was called with, in order: "configure <channels>", "change <old>-><now>".
std::vector<std::string> calls;
bool keepCode = false; // the change version answers with no code

/// Channel c (from 1) plays the value c on every frame, so a channel that keeps playing is one that never changes.
struct Channels {
	void process(Io<spec> io, const GraphCycle&) noexcept {
		float value = 1.0F;
		for (auto member : io[out]) {
			std::ranges::fill(member, value);
			value += 1.0F;
		}
	}
};

struct Shaper {
	using Checked = CheckedOf<spec>;
	static Members<spec> ports(const Checked& s) {
		return {out(members(static_cast<std::size_t>(s.get<channelsRow>())))};
	}
	Configured configure(CreateOf<spec>& cx, const Checked& s) {
		calls.push_back("configure " + std::to_string(s.get<channelsRow>()));
		return cx.applied(Channels{});
	}
	Changed configure(ChangeOf<spec>& cx, const Checked& old, const Checked& now) {
		calls.push_back("change " + std::to_string(old.get<channelsRow>()) + "->" +
		                std::to_string(now.get<channelsRow>()));
		if (keepCode) return cx.applied();
		return cx.applied(Channels{}); // laid out for the new ports
	}
};

using Harness = testing::TypeHarness<spec, Shaper>;

std::unique_ptr<Harness> running(testing::HarnessOptions options = {}) {
	calls.clear();
	keepCode = false;
	auto h = std::make_unique<Harness>(std::move(options));
	REQUIRE_FALSE(h->parse(NodeSettings::object()));
	h->add();
	h->blockReady();
	if (h->phase() != "running") h->node().completeBarrier();
	REQUIRE(h->phase() == "running");
	return h;
}

bool told(Harness& h, NodePhase phase) {
	return std::ranges::any_of(h.node().healths(), [&](const NodeHealth& one) { return one.phase == phase; });
}

/// Every frame of the last cycle of out.<member + 1> is `value`.
bool plays(const Harness& h, std::size_t member, float value) {
	const std::span<const float> frames = h.output(out, member);
	return std::ranges::all_of(frames, [value](float sample) { return sample == value; });
}

} // namespace reshape_test
using namespace reshape_test;

TEST_CASE("A reshape in place keeps channels 1 and 2 playing while a third is added and removed",
          "[reshape][reconfigure]") {
	auto owned = running(testing::HarnessOptions{.autoComplete = false});
	auto& h = *owned;
	for (int i = 0; i < 4; ++i) h.process(); // past the start's fade-in
	REQUIRE(plays(h, 0, 1.0F));
	REQUIRE(plays(h, 1, 2.0F));

	CHECK(h.change({{"channels", 3}}) == NodeOutcome::reshape);
	CHECK(h.phase() == "reconfiguring");
	// Until the commit's swap the old code runs on the ports it was laid out for: channels 1 and 2 at full gain.
	h.process();
	CHECK(plays(h, 0, 1.0F));
	CHECK(plays(h, 1, 2.0F));
	CHECK(h->lastFadeGain("out.1") == 1.0F);
	h.node().completeBarrier(); // the commit's swap
	CHECK(h.phase() == "running");
	h.process();
	CHECK(plays(h, 0, 1.0F));
	CHECK(plays(h, 1, 2.0F));
	CHECK(plays(h, 2, 3.0F));
	CHECK(h->lastFadeGain("out.1") == 1.0F);
	CHECK(h.swaps() == 1);

	CHECK(h.change({{"channels", 2}}) == NodeOutcome::reshape);
	h.node().completeBarrier();
	h.process();
	CHECK(plays(h, 0, 1.0F));
	CHECK(plays(h, 1, 2.0F));
	CHECK_THROWS_AS(h->output("out.3"), std::logic_error); // gone with the layout
	CHECK(calls == std::vector<std::string>{"configure 2", "change 2->3", "change 3->2"});
	CHECK_FALSE(told(h, NodePhase::stopping)); // never restarted
}

TEST_CASE("A reshape with a restart-only key restarts the node behind its block, configured on the new ports",
          "[reshape]") {
	auto owned = running();
	auto& h = *owned;
	CHECK(h.change({{"channels", 3}, {"mode", 1}}) == NodeOutcome::reshape);
	for (int i = 0; i < 4; ++i) h.process(); // past the restart's fade-in
	CHECK(h.phase() == "running");
	CHECK(told(h, NodePhase::stopping));
	// The restart's cause is the reshape's: its keys and the ports it adds (docs/architecture.md#reshapes).
	NodeCause reshape{NodeCause::Kind::reshape, std::nullopt, {"channels", "mode"}};
	reshape.ports = PortsMoved{{"out.3"}, {}};
	NodeCause restarting = reshape;
	restarting.kind = NodeCause::Kind::restarting;
	restarting.restart = NodeCause::Kind::reshape;
	CHECK(std::ranges::any_of(h.node().healths(), [&](const NodeHealth& one) {
		return one.phase == NodePhase::stopping && one.cause == restarting;
	}));
	CHECK(std::ranges::any_of(h.node().healths(), [&](const NodeHealth& one) {
		return one.phase == NodePhase::configuring && one.cause == reshape;
	}));
	CHECK(calls == std::vector<std::string>{"configure 2", "configure 3"});
	CHECK(plays(h, 2, 3.0F));
}

TEST_CASE("A reshape in place whose change version keeps the old code fails the node with the rule's text",
          "[reshape][reconfigure]") {
	auto owned = running();
	auto& h = *owned;
	keepCode = true;
	h.change({{"channels", 3}});
	REQUIRE(h.health().reason.has_value());
	CHECK(h.health().reason->code == FailureReason::Code::lost);
	CHECK(h.health().reason->detail.find("configure changed the ports in place and gave no code for them") !=
	      std::string::npos);
}
