// What must not compile about a settings table and a node type (src/core/CMakeLists.txt registers one CTest test per
// case, each with the reason it must be refused). With no case defined this is the control, and it must compile: what
// the cases break is only what they name.
#include <any>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// As ear_node_type() gives a type's library: a table's name must be its folder's.
#ifdef EAR_CASE_table_not_named_for_folder
#define EAR_NODE_TYPE_NAME "folder"
#endif
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

// The control: a type written the only way there is, reading each row as what it is.
class GoodType final : public NodeTypeOf<table> {
	MadeConfig make(const NodeName&, const Checked& settings) const override {
		const std::uint32_t number = settings.get<numberRow>();
		const std::optional<std::string> text = settings.get<textRow>();
		return std::unexpected(std::string(text.value_or("none")) + std::to_string(number));
	}
};

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

#ifdef EAR_CASE_default_on_text
constexpr auto bad = setting::defaulted("k", setting::text<anyText>("anything"), 1);
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

// A type whose parse skips its table.
#ifdef EAR_CASE_override_parse
class Bad final : public NodeTypeOf<table> {
	MadeConfig make(const NodeName&, const Checked&) const override;
	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings> parse(const NodeName&,
	                                                                        const NodeSettings&) const override;
};
#endif

// A type whose kinds are another table's.
#ifdef EAR_CASE_override_setting_kinds
class Bad final : public NodeTypeOf<table> {
	MadeConfig make(const NodeName&, const Checked&) const override;
	std::span<const SettingDeclaration> settingKinds() const noexcept override { return otherTable.kinds(); }
};
#endif

#ifdef EAR_CASE_override_name
class Bad final : public NodeTypeOf<table> {
	MadeConfig make(const NodeName&, const Checked&) const override;
	std::string_view name() const noexcept override { return "other"; }
};
#endif

#ifdef EAR_CASE_make_takes_raw_settings
class Bad final : public NodeTypeOf<table> {
	MadeConfig make(const NodeName&, const NodeSettings&) const override;
};
#endif

#ifdef EAR_CASE_override_make_config
class Bad final : public NodeTypeOf<table> {
	MadeConfig make(const NodeName&, const Checked&) const override;
	MadeConfig makeConfig(const NodeName&, CheckedSettings) const override;
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

} // namespace settings_table_bad
