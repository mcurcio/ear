// The settings table's walk with rows of its own: each step alone, its order, and what a checked settings object reads.
// What each node type's table says is test/nodes/settings_refusals_test.cpp's.
#include "ear/core/settings_table.hpp"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "catch2/catch.hpp"

namespace {

using ear::CheckedSettings;
using ear::NodeSettings;
using ear::SettingRule;
using ear::SettingsTable;
using enum ear::SettingKind;

int meaningCalls = 0;
int acrossCalls = 0;

bool isGood(std::string_view value) {
	++meaningCalls;
	return value == "good";
}

std::optional<std::string_view> refuseAcross(const NodeSettings&) {
	++acrossCalls;
	return "whole refusal";
}

constexpr SettingRule textRow(std::string_view key, bool required = false) {
	return {.key = key, .kind = text, .required = required, .mustBe = "good, please", .means = isGood};
}

constexpr SettingsTable none{"nothing", std::array<SettingRule, 0>{}};
constexpr SettingsTable one{"t", std::array{textRow("a", true)}};
constexpr SettingsTable two{"t", std::array{textRow("a", true), textRow("b", true)}};
constexpr SettingsTable three{"t",
                              std::array{textRow("a", true), textRow("b", true), textRow("c", true), textRow("d")}};
constexpr SettingsTable optional{"t", std::array{textRow("a"), textRow("b"), textRow("c")}};
constexpr SettingsTable numbers{
	"t",
	std::array{SettingRule{.key = "plain", .kind = wholeNumber, .low = 1, .high = 10},
               SettingRule{.key = "measured", .kind = wholeNumber, .low = 2, .high = 20, .unit = "Hz", .byDefault = 5},
               textRow("name")}};
constexpr SettingsTable required{
	"t", std::array{textRow("a", true), SettingRule{.key = "b", .kind = wholeNumber, .required = true, .high = 9},
                    textRow("c", true)}};
constexpr SettingsTable across{
	"t", std::array{SettingRule{.key = "n", .kind = wholeNumber, .high = 9, .across = refuseAcross}, textRow("z")}};

// The table is a constant, and so is what it declares.
static_assert(numbers.kinds().size() == 3);
static_assert(numbers.kinds()[0].key == "plain" && numbers.kinds()[0].kind == wholeNumber);
static_assert(numbers.kinds()[2].key == "name" && numbers.kinds()[2].kind == text);
static_assert(none.kinds().empty());

template <std::size_t N> std::string refusal(const SettingsTable<N>& table, const NodeSettings& settings) {
	const auto checked = table.check(settings);
	REQUIRE_FALSE(checked.has_value());
	return checked.error();
}

/// JSON text, so a big number is what a parser makes of it.
NodeSettings parsed(std::string_view json) { return NodeSettings::parse(json); }

} // namespace

TEST_CASE("A table with no rows takes null and an empty object, and nothing else", "[settings-table]") {
	const NodeSettings null;
	const NodeSettings empty = NodeSettings::object();
	CHECK(none.check(null).has_value());
	CHECK(none.check(empty).has_value());
	CHECK(refusal(none, 3) == "settings: must be an object");
	CHECK(refusal(none, NodeSettings::array()) == "settings: must be an object");
	CHECK(refusal(none, {{"k", 1}}) == "unknown key k (nothing takes no settings)");
}

TEST_CASE("The object's text names the required keys, or every key when none is required", "[settings-table]") {
	CHECK(refusal(one, nullptr) == R"(settings: must be an object with "a")");
	CHECK(refusal(two, 3) == R"(settings: must be an object with "a" and "b")");
	CHECK(refusal(three, "x") == R"(settings: must be an object with "a", "b" and "c")");
	CHECK(refusal(optional, NodeSettings::array()) == R"(settings: must be an object with optional "a", "b" and "c")");
}

TEST_CASE("Undeclared keys are told first, in the object's own order", "[settings-table]") {
	CHECK(refusal(one, {{"zeta", 1}, {"alpha", 2}}) == "unknown key alpha");
	CHECK(refusal(one, {{"a", 5}, {"zeta", 1}}) == "unknown key zeta");
}

TEST_CASE("Rows are checked in the order declared, and null is given, not missing", "[settings-table]") {
	CHECK(refusal(required, NodeSettings::object()) == "a: missing");
	CHECK(refusal(required, {{"a", "good"}}) == "b: missing");
	CHECK(refusal(required, {{"a", "good"}, {"b", 1}}) == "c: missing");
	CHECK(refusal(required, {{"a", 1}, {"b", 99}}) == "a: must be good, please");
	CHECK(refusal(required, {{"a", "bad"}, {"b", 99}}) == "a: must be good, please");
	CHECK(refusal(required, {{"a", "good"}, {"b", 99}}) == "b: must be a whole number, 0-9");
	CHECK(refusal(required, {{"a", nullptr}}) == "a: must be good, please");
	CHECK(refusal(required, {{"a", "good"}, {"b", nullptr}}) == "b: must be a whole number, 0-9");
	const NodeSettings good = {{"a", "good"}, {"b", 9}, {"c", "good"}};
	CHECK(required.check(good).has_value());
}

TEST_CASE("A whole number is a JSON integer within the row's range", "[settings-table]") {
	const std::string range = "plain: must be a whole number, 1-10";
	for (const std::string_view good : {"1", "10"}) {
		INFO(good);
		const NodeSettings settings = {{"plain", parsed(good)}};
		CHECK(numbers.check(settings).has_value());
	}
	for (const std::string_view bad :
	     {"0", "11", "-1", "1.0", "5.5", "true", "null", R"("5")", "[]", "{}", "4294967297", "9223372036854775807",
	      "9223372036854775808", "18446744073709551615", "18446744073709551616", "-9223372036854775808"}) {
		INFO(bad);
		CHECK(refusal(numbers, {{"plain", parsed(bad)}}) == range);
	}
	CHECK(refusal(numbers, {{"measured", 21}}) == "measured: must be a whole number of Hz, 2-20");
	// Signed as a program makes it, unsigned as the parser does: the same answer.
	const NodeSettings programmed = {{"plain", 5}};
	const NodeSettings byParser = parsed(R"({"plain":5})");
	CHECK(numbers.check(programmed).has_value());
	CHECK(numbers.check(byParser).has_value());
}

TEST_CASE("Meaning is asked of a string only, and its refusal carries mustBe", "[settings-table]") {
	meaningCalls = 0;
	CHECK(refusal(one, {{"a", 5}}) == "a: must be good, please");
	CHECK(refusal(one, {{"a", nullptr}}) == "a: must be good, please");
	CHECK(meaningCalls == 0);
	CHECK(refusal(one, {{"a", "bad"}}) == "a: must be good, please");
	CHECK(meaningCalls == 1);
	const NodeSettings good = {{"a", "good"}};
	CHECK(one.check(good).has_value());
	CHECK(meaningCalls == 2);
}

TEST_CASE("The across rule runs only when its key is given, before its kind, and its text is whole",
          "[settings-table]") {
	acrossCalls = 0;
	const NodeSettings absent = {{"z", "good"}};
	CHECK(across.check(absent).has_value());
	CHECK(acrossCalls == 0);
	CHECK(refusal(across, {{"n", "not a number"}, {"z", 5}}) == "whole refusal");
	CHECK(acrossCalls == 1);
	CHECK(refusal(across, {{"n", nullptr}}) == "whole refusal");
	CHECK(acrossCalls == 2);
}

TEST_CASE("Checked settings read a given value, a default, or nothing", "[settings-table]") {
	const NodeSettings settings = {{"plain", 7}, {"name", "good"}};
	const auto checked = numbers.check(settings);
	REQUIRE(checked.has_value());
	CHECK(checked->wholeNumber("plain") == 7U);
	CHECK(checked->wholeNumber("measured") == 5U);
	CHECK(checked->text("name") == "good");

	const NodeSettings empty = NodeSettings::object();
	const auto bare = numbers.check(empty);
	REQUIRE(bare.has_value());
	CHECK_FALSE(bare->wholeNumber("plain").has_value());
	CHECK(bare->wholeNumber("measured") == 5U);
	CHECK_FALSE(bare->text("name").has_value());

	const NodeSettings overridden = {{"measured", 20}};
	CHECK(numbers.check(overridden)->wholeNumber("measured") == 20U);
}

TEST_CASE("Reading a key the table lacks, or as the other kind, is a type's bug", "[settings-table]") {
	const NodeSettings settings = {{"plain", 7}, {"name", "good"}};
	const auto checked = numbers.check(settings);
	REQUIRE(checked.has_value());
	CHECK_THROWS_AS(checked->text("nope"), std::logic_error);
	CHECK_THROWS_AS(checked->wholeNumber("nope"), std::logic_error);
	CHECK_THROWS_AS(checked->text("plain"), std::logic_error);
	CHECK_THROWS_AS(checked->wholeNumber("name"), std::logic_error);
	const NodeSettings null;
	const auto nothing = none.check(null);
	REQUIRE(nothing.has_value());
	CHECK_THROWS_AS(nothing->text("a"), std::logic_error);
}

TEST_CASE("kinds() is every key once, in row order", "[settings-table]") {
	std::vector<std::string_view> keys;
	for (const auto& [key, kind] : three.kinds()) keys.push_back(key);
	CHECK(keys == std::vector<std::string_view>{"a", "b", "c", "d"});
	CHECK(three.kinds().data() == three.kinds().data());
	CHECK(required.kinds()[1].kind == wholeNumber);
}
