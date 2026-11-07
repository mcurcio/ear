// Every node type through the harness (docs/design/node-framework.md section 7.6), as pluginval and clap-validator run
// every plugin: each type's valid settings parse, start acquires only through its context, every outside resource made
// ready runs the node, every output sample of every cycle is written whatever the inputs hold, and the run closes. The
// types come from the generated list; this file names none (all_types.hpp says which run here).
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <variant>

#include "all_types.hpp"
#include "catch2/catch.hpp"
#include "ear/testing/node_harness.hpp"

TEST_CASE("Every type runs through the harness: starts, runs, writes every output sample and closes", "[harness]") {
	using State = ear::testing::NodeHarness::State;
	constexpr ear::AudioFormat format{.rate = 48000, .maxBlock = 128};
	for (const auto& type : ear::test::allTypes()) {
		INFO(type->name());
		if (!ear::test::runsInHarness(type->name())) continue;
		for (const ear::NodeSettings& settings : ear::test::validSettings(type->name())) {
			INFO(settings.dump());
			ear::testing::NodeHarness harness(*type, format, &ear::test::recordings(), "probe");
			REQUIRE(harness.parse(settings));
			const auto started = harness.start();
			INFO((started ? std::string() : started.error().detail));
			REQUIRE(started);
			for (std::size_t outside = 0; outside < harness.outsideCount(); ++outside) {
				CHECK(harness.outsideName(outside).starts_with("probe."));
				harness.ready(outside);
			}
			REQUIRE(harness.state() == State::running);
			CHECK(harness.notes().empty());

			// Silence, then a full-scale constant, then a short cycle: every output sample written each time.
			for (const float level : {0.0F, 1.0F, -1.0F}) {
				for (const auto* names : {&harness.ports().inputs(), &harness.ports().insideInputs()})
					for (const ear::PortName& port : *names) std::ranges::fill(harness.input(port), level);
				harness.process({.startNs = 0, .position = 0}, format.maxBlock);
				CHECK(harness.unwritten() == 0);
			}
			harness.process({.startNs = 0, .position = format.maxBlock * 3}, 1);
			CHECK(harness.unwritten() == 0);

			harness.close();
			CHECK(harness.state() == State::closed);
		}
	}
}

TEST_CASE("Every type's refusal is a parse refusal in the harness too, before anything starts", "[harness]") {
	for (const auto& type : ear::test::allTypes()) {
		INFO(type->name());
		ear::testing::NodeHarness harness(*type);
		const auto parsed = harness.parse(ear::NodeSettings(3));
		REQUIRE_FALSE(parsed);
		CHECK(parsed.error().starts_with("settings: must be an object"));
		CHECK(harness.state() == ear::testing::NodeHarness::State::unparsed);
	}
}
