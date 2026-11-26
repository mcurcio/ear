// What must not compile about a settings table and a node type (src/core/CMakeLists.txt registers one CTest test per
// case, each with the reason it must be refused). With no case defined this is the control, and it must compile: what
// the cases break is only what they name.
#include <any>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ear/core/settings_table.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace settings_table_bad {

using namespace ear;

std::optional<std::string> anyText(std::string_view text) { return std::string(text); }

constexpr auto numberRow = setting::required("n", setting::number(1, 2));
constexpr auto textRow = setting::optional("t", setting::text<anyText>("anything"));
constexpr auto otherRow = setting::optional("o", setting::text<anyText>("anything"));
constexpr auto optionalNumberRow = setting::optional("m", setting::number(1, 2));
constexpr SettingsTable<numberRow, textRow, optionalNumberRow> table{"t"};
constexpr SettingsTable<otherRow> otherTable{"o"};

using Checked = decltype(table)::Checked;

// The control also holds defaults that must compile: an int literal on a whole number's and on a decimal's rule, the
// default not deduced from it.
constexpr auto intDefault = setting::defaulted("i", setting::number(0, 64, "channels"), 0);
constexpr auto decimalIntDefault = setting::defaulted("g", setting::decimal(-90, 24, "dB"), 0);
constexpr auto flagDefault = setting::defaulted("f", setting::boolean(), false);
static_assert(intDefault.row().byDefault == SettingValue(0u) &&
              decimalIntDefault.row().byDefault == SettingValue(0.0) &&
              flagDefault.row().byDefault == SettingValue(false));

// The control: checked settings read the only way there is, each row as what it is.
std::string good(const Checked& settings) {
	const std::uint32_t number = settings.get<numberRow>();
	const std::optional<std::string> text = settings.get<textRow>();
	return std::string(text.value_or("none")) + std::to_string(number);
}

// The control holds a family too, read by member and counted (docs/writing-a-node-type.md#indexed-families).
constexpr auto countRow = setting::defaulted("c", setting::number(1, 8), 2).inPlace();
constexpr auto familyRow = setting::indexed("f", countRow, setting::decimal(-1, 1), 0).inPlace();
constexpr SettingsTable<countRow, familyRow> familyTable{"f"};
double goodFamily(const decltype(familyTable)::Checked& settings) {
	return settings.get<familyRow>(0) + settings.count<familyRow>();
}

// The control holds a choice too, read as its enumeration, its default named by an enumerator
// (docs/writing-a-node-type.md#choice-rows).
enum class Shape { peak, shelf };
constexpr auto choiceRow =
	setting::defaulted("s", setting::choice<Shape>({{"peak", Shape::peak}, {"low shelf", Shape::shelf}}), Shape::shelf)
		.inPlace();
constexpr SettingsTable<choiceRow> choiceTable{"c"};
Shape goodChoice(const decltype(choiceTable)::Checked& settings) { return settings.get<choiceRow>(); }

#ifdef EAR_CASE_key_empty
constexpr auto bad = setting::required("", setting::number(1, 2));
#endif

#ifdef EAR_CASE_key_reserved_disabled
constexpr auto bad = setting::optional("disabled", setting::text<anyText>("true or false"));
#endif

#ifdef EAR_CASE_key_reserved_disabled_family
constexpr auto bad = setting::required("disabled.1", setting::number(0, 1));
#endif

#ifdef EAR_CASE_key_twice
constexpr auto twin = setting::required("n", setting::number(1, 2));
constexpr SettingsTable<numberRow, twin> bad{"t"};
#endif

#ifdef EAR_CASE_name_empty
constexpr SettingsTable<numberRow> bad{""};
#endif

#ifdef EAR_CASE_low_above_high
constexpr auto bad = setting::number(2, 1);
#endif

#ifdef EAR_CASE_rate_low_above_high
constexpr auto bad = setting::rate(SampleRate::fixed<192'000>(), SampleRate::fixed<8'000>());
#endif

// A number row with no range is what refused almost everything before the rules were two.
#ifdef EAR_CASE_number_without_range
constexpr auto bad = setting::number(1);
#endif

#ifdef EAR_CASE_number_with_mustBe
constexpr auto bad = setting::number(1, 2, "units", "what it must be");
#endif

#ifdef EAR_CASE_default_outside_range
constexpr auto bad = setting::defaulted("k", setting::number(1, 2), 3);
#endif

// defaultFrom says where an absent value comes from: a required or defaulted row is never absent.
#ifdef EAR_CASE_default_from_on_required
constexpr auto bad = setting::required("r", setting::number(1, 2)).defaultFrom(setting::DefaultFrom::nodeName);
#endif

#ifdef EAR_CASE_default_from_on_defaulted
constexpr auto bad =
	setting::defaulted("d", setting::number(1, 2), 1).defaultFrom(setting::DefaultFrom::graphFrequency);
#endif

#ifdef EAR_CASE_help_empty
constexpr auto bad = setting::optional("h", setting::number(1, 2)).help("");
#endif

// A select offering "" offers a value no text row's mustBe names.
#ifdef EAR_CASE_text_hint_empty_value
inline constexpr std::array<std::string_view, 2> withEmpty{"a", ""};
constexpr auto bad = setting::text<anyText>("anything", {.values = withEmpty});
#endif

// A text or a rate is required or optional: its absence has no one value a default could name.
#ifdef EAR_CASE_default_text_row
constexpr auto bad = setting::defaulted("k", setting::text<anyText>("anything"), std::string("x"));
#endif

#ifdef EAR_CASE_default_rate_row
constexpr auto bad = setting::defaulted("k", setting::rate(SampleRate::fixed<8'000>(), SampleRate::fixed<96'000>()),
                                        SampleRate::fixed<48'000>());
#endif

#ifdef EAR_CASE_decimal_low_above_high
constexpr auto bad = setting::decimal(2.0, 1.0);
#endif

#ifdef EAR_CASE_decimal_bound_not_finite
constexpr auto bad = setting::decimal(0.0, std::numeric_limits<double>::infinity());
#endif

#ifdef EAR_CASE_decimal_default_outside
constexpr auto bad = setting::defaulted("k", setting::decimal(0.0, 1.0), 1.5);
#endif

// A log taper maps travel on ln(v): 0 and below have none.
#ifdef EAR_CASE_log_taper_nonpositive
constexpr auto bad = setting::decimal(0.0, 20'000.0, "Hz", {.taper = setting::Taper::log});
#endif

// A dB taper draws a level from near silence to unity at least.
#ifdef EAR_CASE_db_taper_range
constexpr auto bad = setting::decimal(-20.0, 6.0, "dB", {.taper = setting::Taper::db});
#endif

#ifdef EAR_CASE_default_on_required
constexpr auto bad = setting::required("k", setting::number(1, 2), 1);
#endif

#ifdef EAR_CASE_text_without_mustBe
constexpr auto bad = setting::text<anyText>("");
#endif

#ifdef EAR_CASE_text_without_meaning
constexpr auto bad = setting::text<nullptr>("anything");
#endif

#ifdef EAR_CASE_text_with_range
constexpr auto bad = setting::text<anyText>(1, 2);
#endif

// A value that views the text would outlive it.
#ifdef EAR_CASE_text_viewing_value
std::optional<std::string_view> viewing(std::string_view text) { return text; }
constexpr auto bad = setting::text<viewing>("anything");
#endif

#ifdef EAR_CASE_only_without_lower
constexpr auto lower = setting::optional("l", setting::number(1, 2));
constexpr auto upper = setting::optional("u", setting::number(1, 2)).onlyWithout(lower);
constexpr SettingsTable<upper, lower> bad{"t"};
#endif

// A row made by hand skips every check its makers do.
#ifdef EAR_CASE_raw_row
constexpr SettingRow raw{"k", false, WholeNumberRule{2, 1, {}}};
constexpr SettingsTable<raw> bad{"t"};
#endif

#ifdef EAR_CASE_read_unlisted_row
std::optional<std::string> bad(const Checked& settings) { return settings.get<otherRow>(); }
#endif

#ifdef EAR_CASE_read_number_as_text
std::string bad(const Checked& settings) {
	std::string text = settings.get<numberRow>();
	return text;
}
#endif

// An optional row read as a plain value: the read that could fail unseen.
#ifdef EAR_CASE_read_optional_unchecked
std::uint32_t bad(const Checked& settings) {
	std::uint32_t number = settings.get<optionalNumberRow>();
	return number;
}
#endif

// A type whose parse skips its table: NodeType's parse, kinds and name are not virtual, so nothing derived can answer
// for them (and nothing but makeNodeType's type can be constructed: derive_without_table).
#ifdef EAR_CASE_override_parse
class Bad final : public NodeType {
	MadeConfig makeConfig(const NodeName&, CheckedSettings) const override;
	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings> parse(const NodeName&,
	                                                                        const NodeSettings&) const override;
};
#endif

// A type whose kinds are another table's.
#ifdef EAR_CASE_override_setting_kinds
class Bad final : public NodeType {
	MadeConfig makeConfig(const NodeName&, CheckedSettings) const override;
	std::span<const SettingDeclaration> settingKinds() const noexcept override { return otherTable.kinds(); }
};
#endif

#ifdef EAR_CASE_override_name
class Bad final : public NodeType {
	MadeConfig makeConfig(const NodeName&, CheckedSettings) const override;
	std::string_view name() const noexcept override { return "other"; }
};
#endif

#ifdef EAR_CASE_derive_without_table
class Bad final : public NodeType {
public:
	explicit Bad(const AnySettingsTable& table) : NodeType(table) {}

private:
	MadeConfig makeConfig(const NodeName&, CheckedSettings) const override;
};
#endif

#ifdef EAR_CASE_derive_by_copy
class Bad final : public NodeType {
public:
	explicit Bad(const NodeType& real) : NodeType(real) {}

private:
	MadeConfig makeConfig(const NodeName&, CheckedSettings) const override;
};
#endif

#ifdef EAR_CASE_forged_checked_settings
const CheckedSettings bad{std::vector<std::any>{}};
#endif

#ifdef EAR_CASE_forged_row
constexpr setting::Row<std::uint32_t, true> bad("k", true, WholeNumberRule{1, 2, {}});
#endif

// A rule made by hand has no range, or one back to front, or a reader that gives another type than the row reads.
#ifdef EAR_CASE_forged_empty_rule
constexpr setting::Rule<std::uint32_t> bad{};
#endif

#ifdef EAR_CASE_forged_number_rule
constexpr setting::Rule<std::uint32_t> bad{WholeNumberRule{9, 1, {}}};
#endif

#ifdef EAR_CASE_forged_text_rule
constexpr setting::Rule<std::string> bad{TextRule{"x", nullptr}};
#endif

// A default belongs to a row, whose maker checks it against the range.
#ifdef EAR_CASE_default_inside_rule
constexpr WholeNumberRule bad{1, 2, {}, 99};
#endif

// A made row keeps what its maker checked.
#ifdef EAR_CASE_row_field_changed
constexpr auto bad = [] {
	auto row = setting::required("k", setting::number(1, 2));
	row.required = false;
	return row;
}();
#endif

#ifdef EAR_CASE_row_read_only
constexpr auto bad = [] {
	auto row = setting::required("k", setting::number(1, 2));
	row.row().key = "";
	return row;
}();
#endif

// Tables that refuse every object, or hold a row that can never be given.
#ifdef EAR_CASE_only_without_on_required
constexpr auto bad = setting::required("r", setting::number(1, 2)).onlyWithout(textRow);
#endif

#ifdef EAR_CASE_only_without_a_required
constexpr auto bad = setting::optional("o", setting::number(1, 2)).onlyWithout(numberRow);
#endif

// One change mark per row, and a restart says why (docs/writing-a-node-type.md#change-marks).
#ifdef EAR_CASE_two_marks
constexpr auto bad = setting::optional("o", setting::number(1, 2)).inPlace().restarts("opened once");
#endif

#ifdef EAR_CASE_restarts_empty_reason
constexpr auto bad = setting::optional("o", setting::number(1, 2)).restarts("");
#endif

// Indexed families (docs/writing-a-node-type.md#indexed-families).
#ifdef EAR_CASE_indexed_count_below
constexpr auto laterCount = setting::defaulted("later", setting::number(1, 8), 2).inPlace();
constexpr auto earlyFamily = setting::indexed("e", laterCount, setting::number(0, 9), 0).inPlace();
constexpr SettingsTable<earlyFamily, laterCount> bad{"t"};
#endif

#ifdef EAR_CASE_indexed_count_not_whole
constexpr auto flagCount = setting::defaulted("c", setting::boolean(), true);
constexpr auto bad = setting::indexed("e", flagCount, setting::number(0, 9), 0);
#endif

#ifdef EAR_CASE_indexed_count_optional
constexpr auto bad = setting::indexed("e", optionalNumberRow, setting::number(0, 9), 0);
#endif

#ifdef EAR_CASE_indexed_collides_with_row
constexpr auto memberSpelled = setting::optional("f.2", setting::number(0, 9)).inPlace();
constexpr SettingsTable<countRow, familyRow, memberSpelled> bad{"t"};
#endif

#ifdef EAR_CASE_read_family_without_member
double bad(const decltype(familyTable)::Checked& settings) { return settings.get<familyRow>(); }
#endif

#ifdef EAR_CASE_read_member_of_plain_row
std::uint32_t bad(const Checked& settings) { return settings.get<numberRow>(0); }
#endif

// Choice rows (docs/writing-a-node-type.md#choice-rows).
#ifdef EAR_CASE_choice_empty
constexpr auto bad = setting::choice<Shape>({});
#endif

#ifdef EAR_CASE_choice_repeated_name
constexpr auto bad = setting::choice<Shape>({{"peak", Shape::peak}, {"peak", Shape::shelf}});
#endif

#ifdef EAR_CASE_choice_repeated_value
constexpr auto bad = setting::choice<Shape>({{"peak", Shape::peak}, {"top", Shape::peak}});
#endif

#ifdef EAR_CASE_choice_unwritable_name
constexpr auto bad = setting::choice<Shape>({{"peak", Shape::peak}, {"shelf\x01", Shape::shelf}});
#endif

#ifdef EAR_CASE_choice_too_many
enum class Many : int {};
constexpr auto bad = setting::choice<Many>(
	{{"a", Many{0}},  {"b", Many{1}},  {"c", Many{2}},  {"d", Many{3}},  {"e", Many{4}},  {"f", Many{5}},
     {"g", Many{6}},  {"h", Many{7}},  {"i", Many{8}},  {"j", Many{9}},  {"k", Many{10}}, {"l", Many{11}},
     {"m", Many{12}}, {"n", Many{13}}, {"o", Many{14}}, {"p", Many{15}}, {"q", Many{16}}, {"r", Many{17}},
     {"s", Many{18}}, {"t", Many{19}}, {"u", Many{20}}, {"v", Many{21}}, {"w", Many{22}}, {"x", Many{23}},
     {"y", Many{24}}, {"z", Many{25}}, {"A", Many{26}}, {"B", Many{27}}, {"C", Many{28}}, {"D", Many{29}},
     {"E", Many{30}}, {"F", Many{31}}, {"G", Many{32}}});
#endif

#ifdef EAR_CASE_choice_default_unnamed
constexpr auto bad = setting::defaulted("k", setting::choice<Shape>({{"peak", Shape::peak}}), Shape::shelf);
#endif

#ifdef EAR_CASE_choice_not_enum
constexpr auto bad = setting::choice<int>({{"one", 1}});
#endif

} // namespace settings_table_bad
