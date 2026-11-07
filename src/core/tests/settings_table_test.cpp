// The settings table's walk with rows of its own: each step alone, its order, what a checked settings object reads, and
// how NodeType answers from its table. What each node type's table says is test/nodes/settings_refusals_test.cpp's; what
// must not compile is compile_fail/settings_table_bad.cpp's.
#include "ear/core/settings_table.hpp"

#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "catch2/catch.hpp"

namespace {

using ear::MadeConfig;
using ear::NodeName;
using ear::NodeSettings;
using ear::SettingsTable;
using enum ear::SettingKind;
namespace setting = ear::setting;

int meaningCalls = 0;

std::optional<std::string> isGood(std::string_view value) {
	++meaningCalls;
	if (value != "good") return std::nullopt;
	return std::string(value);
}

constexpr auto goodText = setting::text<isGood>("good, please");
constexpr auto aRow = setting::required("a", goodText);
constexpr auto bRow = setting::required("b", goodText);
constexpr auto cRow = setting::required("c", goodText);
constexpr auto dRow = setting::optional("d", goodText);
constexpr auto aOptional = setting::optional("a", goodText);
constexpr auto bOptional = setting::optional("b", goodText);
constexpr auto cOptional = setting::optional("c", goodText);
constexpr auto plainRow = setting::optional("plain", setting::number(1, 10));
constexpr auto measuredRow = setting::defaulted("measured", setting::number(2, 20, "Hz"), 5);
constexpr auto nameRow = setting::optional("name", goodText);
constexpr auto bNumber = setting::required("b", setting::number(0, 9));
constexpr auto mRow = setting::optional("m", goodText);
constexpr auto nRow = setting::optional("n", setting::number(0, 9)).onlyWithout(mRow);
constexpr auto zRow = setting::optional("z", goodText);

constexpr SettingsTable<> none{"nothing"};
constexpr SettingsTable<aRow> one{"t"};
constexpr SettingsTable<aRow, bRow> two{"t"};
constexpr SettingsTable<aRow, bRow, cRow, dRow> three{"t"};
constexpr SettingsTable<aOptional, bOptional, cOptional> optional{"t"};
constexpr SettingsTable<plainRow, measuredRow, nameRow> numbers{"t"};
constexpr SettingsTable<aRow, bNumber, cRow> required{"t"};
constexpr SettingsTable<mRow, nRow, zRow> across{"t"};

// The table is a constant, and so is what it declares.
static_assert(numbers.kinds().size() == 3);
static_assert(numbers.kinds()[0].key == "plain" && numbers.kinds()[0].kind == wholeNumber);
static_assert(numbers.kinds()[2].key == "name" && numbers.kinds()[2].kind == text);
static_assert(none.kinds().empty());

template <class Table> std::string refusal(const Table& table, const NodeSettings& settings) {
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
	const NodeSettings absent = {{"m", "good"}, {"z", "good"}};
	CHECK(across.check(absent).has_value());
	const NodeSettings alone = {{"n", 5}, {"z", "good"}};
	CHECK(across.check(alone).has_value());
	CHECK(refusal(across, {{"m", "good"}, {"n", "not a number"}, {"z", 5}}) == R"(n: only without "m")");
	CHECK(refusal(across, {{"m", "good"}, {"n", nullptr}}) == R"(n: only without "m")");
	// The row above is checked first, and its absence leaves the row's own kind to be looked at.
	CHECK(refusal(across, {{"m", 5}, {"n", 1}}) == "m: must be good, please");
	CHECK(refusal(across, {{"n", "not a number"}}) == "n: must be a whole number, 0-9");
}

TEST_CASE("Checked settings read a given value, a default, or nothing", "[settings-table]") {
	const NodeSettings settings = {{"plain", 7}, {"name", "good"}};
	const auto checked = numbers.check(settings);
	REQUIRE(checked.has_value());
	CHECK(checked->get<plainRow>() == 7U);
	CHECK(checked->get<measuredRow>() == 5U);
	CHECK(checked->get<nameRow>() == "good");

	const NodeSettings empty = NodeSettings::object();
	const auto bare = numbers.check(empty);
	REQUIRE(bare.has_value());
	CHECK_FALSE(bare->get<plainRow>().has_value());
	CHECK(bare->get<measuredRow>() == 5U);
	CHECK_FALSE(bare->get<nameRow>().has_value());

	const NodeSettings overridden = {{"measured", 20}};
	CHECK(numbers.check(overridden)->get<measuredRow>() == 20U);
}

using NumbersChecked = decltype(numbers)::Checked;
template <const auto& Row>
concept readable = requires(const NumbersChecked& checked) { checked.template get<Row>(); };

TEST_CASE("Reading a key the table lacks, or as the other kind, is a type's bug", "[settings-table]") {
	// A key the table lacks does not compile, and neither does the other kind (compile_fail's read-number-as-text): each
	// row reads as its own type and no other.
	static_assert(readable<plainRow> && readable<measuredRow> && readable<nameRow>);
	static_assert(!readable<aRow> && !readable<zRow>);
	const NodeSettings settings = {{"plain", 7}, {"name", "good"}};
	const auto checked = numbers.check(settings);
	REQUIRE(checked.has_value());
	static_assert(std::same_as<decltype(checked->get<plainRow>()), std::optional<std::uint32_t>>);
	static_assert(std::same_as<decltype(checked->get<measuredRow>()), std::uint32_t>);
	static_assert(std::same_as<decltype(checked->get<nameRow>()), std::optional<std::string>>);
	CHECK(checked->get<plainRow>() == 7U);
}

TEST_CASE("kinds() is every key once, in row order", "[settings-table]") {
	std::vector<std::string_view> keys;
	for (const auto& [key, kind] : three.kinds()) keys.push_back(key);
	CHECK(keys == std::vector<std::string_view>{"a", "b", "c", "d"});
	CHECK(three.kinds().data() == three.kinds().data());
	CHECK(required.kinds()[1].kind == wholeNumber);
}

TEST_CASE("A text's meaning is asked once and its value read without asking again", "[settings-table]") {
	const NodeSettings settings = {{"a", "good"}};
	meaningCalls = 0;
	const auto checked = one.check(settings);
	REQUIRE(checked.has_value());
	CHECK(meaningCalls == 1);
	CHECK(checked->get<aRow>() == "good");
	CHECK(checked->get<aRow>() == "good");
	CHECK(meaningCalls == 1);
}

namespace {

struct Doubled {
	int value;
};
std::optional<Doubled> doubled(std::string_view text) {
	if (text != "21") return std::nullopt;
	return Doubled{42};
}
constexpr auto doubledRow = setting::required("x", setting::text<doubled>("21"));
constexpr auto maybeDoubledRow = setting::optional("y", setting::text<doubled>("21"));
constexpr SettingsTable<doubledRow, maybeDoubledRow> doubledTable{"t"};

} // namespace

TEST_CASE("A row reads as its type", "[settings-table]") {
	const NodeSettings settings = {{"x", "21"}};
	const auto checked = doubledTable.check(settings);
	REQUIRE(checked.has_value());
	static_assert(std::same_as<decltype(checked->get<doubledRow>()), Doubled>); // a copy: nothing to dangle
	static_assert(std::same_as<decltype(checked->get<maybeDoubledRow>()), std::optional<Doubled>>);
	CHECK(checked->get<doubledRow>().value == 42);
	CHECK_FALSE(checked->get<maybeDoubledRow>().has_value());
	CHECK(refusal(doubledTable, {{"x", "22"}}) == "x: must be 21");
}

TEST_CASE("A read from temporary checked settings outlives them", "[settings-table]") {
	static_assert(std::same_as<decltype(one.check(NodeSettings{})->get<aRow>()), std::string>);
	const std::string& kept = one.check(NodeSettings{{"a", "good"}})->get<aRow>();
	CHECK(kept == "good");
}

TEST_CASE("Checked settings own their values, past the table and the settings", "[settings-table]") {
	const auto checked = SettingsTable<aRow, measuredRow>{"t"}.check(NodeSettings{{"a", "good"}, {"measured", 7}});
	REQUIRE(checked.has_value());
	CHECK(checked->get<aRow>() == "good");
	CHECK(checked->get<measuredRow>() == 7U);
}

namespace {

class NoPorts final : public ear::NodeConfig {
public:
	const ear::Ports& ports() const override { return ports_; }
	ear::StartResult start(ear::StartContext&) const override {
		return std::unexpected(ear::FailureReason{ear::FailureReason::Code::startFailed, "not started in these tests"});
	}

private:
	ear::Ports ports_;
};

/// Counts every call of make, and refuses when told to.
class CountingType final : public ear::NodeTypeOf<numbers> {
public:
	mutable int made = 0;
	mutable NodeName madeFor;
	bool refuse = false;

private:
	MadeConfig make(const NodeName& node, const Checked& settings) const override {
		++made;
		madeFor = node;
		if (refuse) return std::unexpected("make says no to " + std::to_string(*settings.get<plainRow>()));
		return std::make_unique<const NoPorts>();
	}
};

} // namespace

TEST_CASE("A type's name, kinds and parse come from its table", "[settings-table]") {
	const CountingType type;
	const ear::NodeType& asType = type;
	CHECK(asType.name() == "t");
	CHECK(asType.settingKinds().data() == numbers.kinds().data());
	CHECK(asType.settingKinds().size() == numbers.kinds().size());
	const auto refused = asType.parse("node", {{"unknown", 1}});
	REQUIRE_FALSE(refused.has_value());
	CHECK(refused.error().node == "node");
	CHECK(refused.error().detail == "unknown key unknown");
	CHECK(asType.parse("node", NodeSettings::object()).has_value());
}

TEST_CASE("A refusal never reaches make, and a refusal from make carries the node", "[settings-table]") {
	CountingType type;
	CHECK_FALSE(type.parse("node", {{"plain", 99}}).has_value());
	CHECK(type.made == 0);

	CHECK(type.parse("node", {{"plain", 3}}).has_value());
	CHECK(type.made == 1);
	CHECK(type.madeFor == "node");

	type.refuse = true;
	const auto refused = type.parse("other", {{"plain", 3}});
	REQUIRE_FALSE(refused.has_value());
	CHECK(type.made == 2);
	CHECK(refused.error().node == "other");
	CHECK(refused.error().detail == "make says no to 3");
}
