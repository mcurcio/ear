// Every type's live rows through the harness (docs/writing-a-node-type.md#what-every-type-must-also-pass;
// docs/writing-a-node-type.md#live-rows): each moved to its minimum and its maximum is a retune that leaves the ports
// and the node as they were; each ramped row ramps from its default to its maximum with every output sample written;
// each row whose minimum is silence (minIsSilence) gives exact zeros there, whatever the other live rows hold. The
// types come from the generated list; this file names none, and a sweep that finds no type with a live row fails.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "all_types.hpp"
#include "catch2/catch.hpp"
#include "ear/core/settings_table.hpp"
#include "ear/testing/node_harness.hpp"

namespace {

constexpr ear::AudioFormat format{.rate = ear::SampleRate::fixed<48000>(), .maxBlock = 128};

/// A live row's two ends as settings values: a decimal's or a whole number's range, a boolean's false and true, a
/// choice's first and last name.
std::pair<ear::NodeSettings, ear::NodeSettings> endsOf(const ear::SettingRow& row) {
	if (const auto* decimal = std::get_if<ear::DecimalRule>(&row.rule)) return {decimal->low, decimal->high};
	if (const auto* number = std::get_if<ear::WholeNumberRule>(&row.rule)) return {number->low, number->high};
	if (const auto* choice = std::get_if<ear::ChoiceRule>(&row.rule))
		return {std::string(choice->taken().front()), std::string(choice->taken().back())};
	return {false, true};
}

/// The keys of a live row a sweep moves: a plain row's own; a family's first member and its last active one.
std::vector<std::string> keysOf(const ear::SettingRow& row, const ear::TypeSchema& schema,
                                const ear::NodeSettings& settings) {
	if (row.maxMembers == 0) return {std::string(row.key)};
	std::uint32_t count = 0;
	for (const ear::SettingRow* other : schema.settings) {
		if (other->key != row.indexedBy) continue;
		if (const auto given = settings.find(std::string(other->key)); given != settings.end())
			count = given->get<std::uint32_t>();
		else if (other->byDefault) count = std::get<std::uint32_t>(*other->byDefault);
	}
	std::vector<std::string> keys{std::string(row.key) + ".1"};
	if (count > 1) keys.push_back(std::string(row.key) + "." + std::to_string(count));
	return keys;
}

/// Every key of the type's live rows, every family member up to its maximum: what a sweep sets to put every live row
/// at one end.
std::vector<std::pair<std::string, const ear::SettingRow*>> everyLiveKey(const ear::TypeSchema& schema) {
	std::vector<std::pair<std::string, const ear::SettingRow*>> keys;
	for (const ear::SettingRow* row : schema.settings) {
		if (!ear::setting::isLive(*row)) continue;
		if (row->maxMembers == 0) keys.emplace_back(std::string(row->key), row);
		for (std::uint32_t member = 1; member <= row->maxMembers; ++member)
			keys.emplace_back(std::string(row->key) + "." + std::to_string(member), row);
	}
	return keys;
}

/// Every input, public and inside, holds `level`.
void feed(ear::testing::NodeHarness& harness, float level) {
	for (const auto* names : {&harness.ports().inputs(), &harness.ports().insideInputs()})
		for (const ear::PortName& port : *names) std::ranges::fill(harness.input(port), level);
}

/// Every companion of the newest configuration made ready.
void readyEvery(ear::testing::NodeHarness& harness) {
	for (const auto& companion : harness.companions())
		if (!companion.destroyed && !companion.ready) harness.ready(companion.bank);
}

/// A node of `type` with `settings`, running, its start's fade-in over.
void running(ear::testing::NodeHarness& harness, const ear::NodeSettings& settings) {
	const auto refused = harness.parse(settings);
	INFO(refused.value_or(""));
	REQUIRE_FALSE(refused);
	harness.add();
	harness.blockReady();
	readyEvery(harness);
	REQUIRE(harness.phase() == "running");
	feed(harness, 0.5F);
	for (int block = 0; block < 3; ++block) harness.process(format.maxBlock); // 384 frames: past the 5 ms fade-in
}

bool anyLiveRow(const ear::TypeSchema& schema) {
	return std::ranges::any_of(schema.settings, [](const ear::SettingRow* row) { return ear::setting::isLive(*row); });
}

} // namespace

TEST_CASE(
	"Every type's live row moved to its minimum and its maximum is a retune with the same ports, no restart, no swap",
	"[harness][live]") {
	std::size_t moved = 0;
	for (const auto& type : ear::test::allTypes()) {
		const ear::TypeSchema schema = type->schema();
		if (!anyLiveRow(schema)) continue;
		INFO(type->name());
		for (const ear::NodeSettings& settings : ear::test::validSettings(type->name()))
			for (const ear::SettingRow* row : schema.settings) {
				if (!ear::setting::isLive(*row)) continue;
				const auto [low, high] = endsOf(*row);
				for (const std::string& key : keysOf(*row, schema, settings))
					for (const ear::NodeSettings& value : {low, high}) {
						ear::NodeSettings to = settings;
						to[key] = value;
						INFO(to.dump());
						ear::testing::NodeHarness harness(*type, {.format = format, .node = "probe"});
						running(harness, settings);
						const ear::Ports ports = harness.ports();
						const std::size_t healths = harness.healths().size();
						const std::size_t swaps = harness.swaps();
						const ear::NodeOutcome outcome = harness.change(to);
						if (to == settings) CHECK(outcome == ear::NodeOutcome::unchanged);
						else CHECK(outcome == ear::NodeOutcome::retune);
						++moved;
						for (int block = 0; block < 8; ++block) {
							harness.process(format.maxBlock);
							CHECK(harness.unwritten() == 0);
						}
						CHECK(harness.phase() == "running");
						CHECK(harness.ports() == ports);
						CHECK(harness.healths().size() == healths);
						CHECK(harness.swaps() == swaps);
						CHECK(harness.diagnostics().empty());
					}
			}
	}
	CHECK(moved > 0); // a sweep over no live row checks nothing (docs/testing.md#the-rules)
}

TEST_CASE("Every type's ramped live row ramps from its default to its maximum with every output sample written",
          "[harness][live]") {
	std::size_t ramped = 0;
	for (const auto& type : ear::test::allTypes()) {
		const ear::TypeSchema schema = type->schema();
		if (!anyLiveRow(schema)) continue;
		INFO(type->name());
		const ear::NodeSettings& valid = ear::test::validSettings(type->name()).front();
		for (const ear::SettingRow* row : schema.settings) {
			if (!ear::setting::isLive(*row) || row->rampMs == 0) continue;
			for (const std::string& key : keysOf(*row, schema, valid)) {
				INFO(key);
				ear::NodeSettings from = valid;
				from.erase(key); // its default
				ear::NodeSettings to = from;
				to[key] = endsOf(*row).second;
				ear::testing::NodeHarness harness(*type, {.format = format, .node = "probe"});
				running(harness, from);
				const std::size_t healths = harness.healths().size();
				CHECK(harness.change(to) == ear::NodeOutcome::retune);
				++ramped;
				// Odd blocks, so the ramp's end falls inside one: from its first frame to past its last.
				const std::uint32_t frames = row->rampMs * 48;
				for (std::uint32_t done = 0; done < frames + 2 * 77; done += 77) {
					harness.process(77);
					REQUIRE(harness.unwritten() == 0);
				}
				CHECK(harness.healths().size() == healths);
			}
		}
	}
	CHECK(ramped > 0);
}

TEST_CASE("Every type's live row whose minimum is silence gives exact zeros there, whatever the other live rows hold",
          "[harness][live]") {
	std::size_t silenced = 0;
	for (const auto& type : ear::test::allTypes()) {
		const ear::TypeSchema schema = type->schema();
		if (!anyLiveRow(schema)) continue;
		INFO(type->name());
		const ear::NodeSettings& valid = ear::test::validSettings(type->name()).front();
		const auto every = everyLiveKey(schema);
		for (const ear::SettingRow* row : schema.settings) {
			const auto* decimal = std::get_if<ear::DecimalRule>(&row->rule);
			if (!ear::setting::isLive(*row) || decimal == nullptr || !decimal->hints.minIsSilence) continue;
			for (const std::string& key : keysOf(*row, schema, valid)) {
				INFO(key);
				// The other live rows at their minimum, then at their maximum, then as the valid settings give them.
				for (const std::optional<bool> others :
				     {std::optional<bool>(false), std::optional<bool>(true), std::optional<bool>()}) {
					ear::NodeSettings loud = valid;
					if (others)
						for (const auto& [other, otherRow] : every) {
							if (otherRow == row) continue;
							const auto [low, high] = endsOf(*otherRow);
							loud[other] = *others ? high : low;
						}
					ear::NodeSettings quiet = loud;
					quiet[key] = decimal->low;
					INFO(quiet.dump());
					// Parsed there from the start (the slots seeded: no ramp at the first drive), and moved there live
					// from the loud settings, its ramp over.
					ear::testing::NodeHarness parsed(*type, {.format = format, .node = "probe"});
					running(parsed, quiet);
					ear::testing::NodeHarness moved(*type, {.format = format, .node = "probe"});
					running(moved, loud);
					CHECK(moved.change(quiet) == ear::NodeOutcome::retune);
					for (int block = 0; block < static_cast<int>(row->rampMs * 48 / format.maxBlock) + 1; ++block)
						moved.process(format.maxBlock);
					for (auto* harness : {&parsed, &moved})
						for (const float level : {1.0F, -1.0F, 0.25F}) {
							feed(*harness, level);
							harness->process(format.maxBlock);
							for (const auto* names : {&harness->ports().outputs(), &harness->ports().insideOutputs()})
								for (const ear::PortName& port : *names) {
									INFO(port);
									CHECK(std::ranges::all_of(harness->output(port),
									                          [](float sample) { return sample == 0.0F; }));
								}
						}
					++silenced;
				}
			}
		}
	}
	CHECK(silenced > 0);
}
