// Every node type through the harness (docs/writing-a-node-type.md#testing-a-type;
// docs/writing-a-node-type.md#testing-a-type: a real NodeHost over fakes), as pluginval and clap-validator run every
// plugin: each type's valid settings parse, the node is added and configured, every companion it asked for made ready
// runs it, every output sample of every cycle is written whatever the inputs hold, and it stops, starts, restarts, is
// disabled, enabled and removed. The types come from the generated list; this file names none (all_types.hpp says which
// do not run here yet).
#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <variant>

#include "all_types.hpp"
#include "catch2/catch.hpp"
#include "ear/testing/node_harness.hpp"

namespace {

/// Silence, then a full-scale constant each way, then a short cycle: every output sample written each time.
void everySampleWritten(ear::testing::NodeHarness& harness, std::uint32_t maxBlock) {
	for (const float level : {0.0F, 1.0F, -1.0F}) {
		for (const auto* names : {&harness.ports().inputs(), &harness.ports().insideInputs()})
			for (const ear::PortName& port : *names) std::ranges::fill(harness.input(port), level);
		harness.process(maxBlock);
		CHECK(harness.unwritten() == 0);
	}
	harness.process(1);
	CHECK(harness.unwritten() == 0);
}

/// Every companion of the newest configuration made ready.
void readyEvery(ear::testing::NodeHarness& harness) {
	for (const auto& companion : harness.companions())
		if (!companion.destroyed && !companion.ready) {
			CHECK(companion.name.starts_with("probe."));
			harness.ready(companion.bank);
		}
}

} // namespace

TEST_CASE("Every type runs through the harness: configures, runs, writes every output sample, and walks its lifecycle",
          "[harness]") {
	constexpr ear::AudioFormat format{.rate = ear::SampleRate::fixed<48000>(), .maxBlock = 128};
	const auto types = ear::test::allTypes();
	for (const auto& name : ear::test::notYetInHarness()) {
		INFO(name << " is in notYetInHarness (test/nodes/all_types.hpp)");
		CHECK(std::ranges::any_of(types, [&](const auto& type) { return type->name() == name; }));
	}
	for (const auto& type : types) {
		INFO(type->name());
		if (ear::test::notYetInHarness().contains(type->name())) continue;
		for (const ear::NodeSettings& settings : ear::test::validSettings(type->name())) {
			INFO(settings.dump());
			ear::testing::NodeHarness harness(
				*type, {.format = format, .node = "probe", .recordings = &ear::test::recordings()});
			const auto refused = harness.parse(settings);
			INFO(refused.value_or(""));
			REQUIRE_FALSE(refused);
			harness.add();
			INFO((harness.health().reason ? harness.health().reason->detail : std::string()));
			REQUIRE(harness.phase() != "failed");
			harness.blockReady();
			readyEvery(harness);
			REQUIRE(harness.phase() == "running");
			CHECK(harness.notes().empty());
			everySampleWritten(harness, format.maxBlock);

			REQUIRE(harness.stop());
			CHECK(harness.phase() == "stopped");
			everySampleWritten(harness, format.maxBlock); // the silent block writes zeros
			REQUIRE(harness.start());
			CHECK(harness.phase() == "running");

			REQUIRE(harness.restart());
			readyEvery(harness);
			CHECK(harness.phase() == "running");
			everySampleWritten(harness, format.maxBlock);

			harness.disable();
			CHECK(harness.phase() == "disabled");
			CHECK(std::ranges::all_of(harness.companions(), [](const auto& made) { return made.destroyed; }));
			harness.enable();
			readyEvery(harness);
			CHECK(harness.phase() == "running");

			harness.remove();
			CHECK(harness.gone());
			CHECK(harness.droppedFrames() == 0);
			CHECK(std::ranges::all_of(harness.companions(), [](const auto& made) { return made.destroyed; }));
			CHECK(harness.diagnostics().empty());
		}
	}
}

TEST_CASE("Every type's refusal is a parse refusal in the harness too, before anything starts", "[harness]") {
	for (const auto& type : ear::test::allTypes()) {
		INFO(type->name());
		ear::testing::NodeHarness harness(*type);
		const auto refused = harness.parse(ear::NodeSettings(3));
		REQUIRE(refused);
		CHECK(refused->starts_with("settings: must be an object"));
		CHECK_THROWS_AS(harness.add(), std::logic_error);
	}
}
