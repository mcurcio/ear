// One node type's settings, each key declared once. The table checks the settings object in one
// fixed order, makes every refusal's text from the row (or carries what cannot be made), and is
// what the type's NodeType::settingKinds() returns, so the graph file's reader cannot disagree
// with the type's parse. The type keeps what a text value means and any rule across settings.
// Pure: nlohmann/json only.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include "ear/core/graph_spec.hpp"
#include "ear/core/node_type.hpp"
namespace ear {
/// Whether `text` is one this type can use; the type's own (CardId, RtpAddress, L16 or L24...).
using TextMeaning = bool (*)(std::string_view text);
/// A rule across settings that one key answers to: the whole refusal's text, or nothing. It reads
/// only keys of rows above its own, which have passed by then.
using AcrossRule = std::optional<std::string_view> (*)(const NodeSettings& settings);

/// One key of one node type. Rows are checked in the order declared, which is also the order of
/// settingKinds(). A key given with any value, null included, is given.
struct SettingRule {
	std::string_view key;
	SettingKind kind;
	bool required = false; // absent: "<key>: missing"; never with byDefault
	// Whole numbers only. A value is good when it is a JSON integer (a float, a bool and a string
	// are not) in [low, high]; an integer above INT64_MAX is above every range. Otherwise:
	// "<key>: must be a whole number[ of <unit>], <low>-<high>".
	std::uint32_t low = 0;
	std::uint32_t high = 0;
	std::string_view unit = {};
	std::optional<std::uint32_t> byDefault = std::nullopt; // what wholeNumber() gives when absent
	// Text only, both needed. A value is good when it is a JSON string `means` accepts. Otherwise:
	// "<key>: must be <mustBe>".
	std::string_view mustBe = {};
	TextMeaning means = nullptr;
	// Either kind: runs when the key is given, before its kind is looked at.
	AcrossRule across = nullptr;
};

/// Settings a table has passed: the only way to read them. Borrows the settings and the table's
/// rows; lives no longer than either, so only for the length of a parse.
class CheckedSettings {
public:
	/// The key's text, nothing when absent. Views the checked settings.
	/// Throws std::logic_error when the table has no such text key: a type bug, not a user error.
	std::optional<std::string_view> text(std::string_view key) const;
	/// The key's number, else its byDefault, else nothing. Throws as text() does.
	std::optional<std::uint32_t> wholeNumber(std::string_view key) const;

private:
	CheckedSettings(std::span<const SettingRule> rows, const NodeSettings& settings) noexcept;
	friend std::expected<CheckedSettings, std::string>
	checkSettings(std::string_view type, std::span<const SettingRule> rows, const NodeSettings& settings);
	std::span<const SettingRule> rows_;
	const NodeSettings* settings_;
};

/// SettingsTable::check's walk; the first failure is the refusal's text, in this order:
/// 1. Not an object: "settings: must be an object", with no rows; else that and
///    ` with "a", "b" and "c"` naming the required keys, or when none is required
///    ` with optional "a", "b" and "c"` naming every key. With no rows, null is taken as {}.
/// 2. An undeclared key, the first in the object's own (sorted) order: "unknown key <key>", with no
///    rows followed by " (<type> takes no settings)".
/// 3. Each row in order, as SettingRule says: absent (next row, or missing when required), then
///    `across`, then the kind and the range or meaning.
std::expected<CheckedSettings, std::string> checkSettings(std::string_view type, std::span<const SettingRule> rows,
                                                          const NodeSettings& settings);

/// A type's table, a constant it declares once:
///     constexpr SettingsTable table{"rtp", std::array{SettingRule{...}, SettingRule{...}}};
/// A table with no rows names its array's size: std::array<SettingRule, 0>{}.
template <std::size_t N> class SettingsTable {
public:
	/// A row that makes no sense does not compile: a key empty or given twice; text without mustBe
	/// and means, or with a range, unit or byDefault; a number with mustBe or means, low above high,
	/// or a byDefault outside its range or on a required row.
	consteval SettingsTable(std::string_view type, const std::array<SettingRule, N>& rows)
		: type_(type), rows_(rows), kinds_{} {
		for (std::size_t at = 0; at < N; ++at) {
			const SettingRule& row = rows[at];
			// A throw in a consteval constructor is what stops the compile; the text is what the compiler shows.
			if (row.key.empty()) throw "a setting's key is empty";
			for (std::size_t earlier = 0; earlier < at; ++earlier) {
				if (rows[earlier].key == row.key) throw "a setting's key is given twice";
			}
			if (row.kind == SettingKind::text) {
				if (row.mustBe.empty() || row.means == nullptr) throw "a text setting needs mustBe and means";
				if (row.low != 0 || row.high != 0 || !row.unit.empty() || row.byDefault) {
					throw "a text setting has no range, unit or byDefault";
				}
			} else {
				if (!row.mustBe.empty() || row.means != nullptr) throw "a whole number has no mustBe or means";
				if (row.low > row.high) throw "a whole number's low is above its high";
				if (row.byDefault && row.required) throw "a required setting has no byDefault";
				if (row.byDefault && (*row.byDefault < row.low || *row.byDefault > row.high)) {
					throw "a whole number's byDefault is outside its range";
				}
			}
			kinds_[at] = SettingDeclaration{row.key, row.kind};
		}
	}
	std::expected<CheckedSettings, std::string> check(const NodeSettings& settings) const {
		return checkSettings(type_, rows_, settings);
	}
	std::expected<CheckedSettings, std::string> check(const NodeSettings&&) const = delete; // it would dangle
	/// NodeType::settingKinds(): every key, each once, in row order; the same span every call.
	constexpr std::span<const SettingDeclaration> kinds() const noexcept { return kinds_; }

private:
	std::string_view type_;
	std::array<SettingRule, N> rows_;
	std::array<SettingDeclaration, N> kinds_;
};
} // namespace ear
