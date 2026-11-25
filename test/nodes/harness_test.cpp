// Every node type through the harness (docs/writing-a-node-type.md#testing-a-type;
// docs/writing-a-node-type.md#testing-a-type: a real NodeHost over fakes), as pluginval and clap-validator run every
// plugin: each type's valid settings parse, the node is added and configured, every companion it asked for made ready
// runs it, every output sample of every cycle is written whatever the inputs hold, and it stops, starts, restarts, is
// disabled, enabled and removed. The types come from the generated list; this file names none.
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
	for (const auto& type : ear::test::allTypes()) {
		INFO(type->name());
		for (const ear::NodeSettings& settings : ear::test::validSettings(type->name())) {
			INFO(settings.dump());
			ear::testing::NodeHarness harness(*type, {.format = format, .node = "probe"});
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

TEST_CASE("Every type's change of keys marked in place ends running with the same configuration, or in a restart "
          "it asked for",
          "[harness][reconfigure]") {
	constexpr ear::AudioFormat format{.rate = ear::SampleRate::fixed<48000>(), .maxBlock = 128};
	std::size_t inPlace = 0;
	for (const auto& type : ear::test::allTypes()) {
		INFO(type->name());
		const auto settings = ear::test::validSettings(type->name());
		for (const ear::NodeSettings& from : settings)
			for (const ear::NodeSettings& to : settings) {
				if (from == to) continue;
				ear::testing::NodeHarness harness(*type, {.format = format, .node = "probe"});
				REQUIRE_FALSE(harness.parse(from));
				const ear::Ports ports = harness.ports();
				if (harness.parse(to) || harness.ports() != ports) continue; // a replace, not a change
				REQUIRE_FALSE(harness.parse(from));
				INFO(from.dump() << " to " << to.dump());
				harness.add();
				harness.blockReady();
				readyEvery(harness);
				REQUIRE(harness.phase() == "running");
				const std::size_t before = harness.healths().size();
				if (harness.change(to) != ear::NodeOutcome::reconfigure) continue; // a restart-only key changed
				++inPlace;
				readyEvery(harness);
				CHECK(harness.phase() == "running");
				for (std::size_t i = before; i < harness.healths().size(); ++i) {
					const ear::NodeHealth& one = harness.healths()[i];
					if (one.phase != ear::NodePhase::stopping) continue;
					REQUIRE(one.cause);
					CHECK(one.cause->restart == ear::NodeCause::Kind::fellBack);
					CHECK(one.cause->reason == ear::NodeCause::FellBack::restartAsked);
				}
				everySampleWritten(harness, format.maxBlock);
			}
	}
	CHECK(inPlace > 0); // some type's valid settings differ in place only
}
