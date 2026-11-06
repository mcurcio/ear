#include "ear/core/settings_table.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ear {
namespace {

/// `"a", "b" and "c"`.
std::string quotedList(const std::vector<std::string_view>& keys) {
	std::string list;
	for (std::size_t at = 0; at < keys.size(); ++at) {
		if (at > 0) list += at + 1 == keys.size() ? " and " : ", ";
		list += '"';
		list += keys[at];
		list += '"';
	}
	return list;
}

std::string notAnObject(std::span<const SettingRule> rows) {
	std::vector<std::string_view> required;
	std::vector<std::string_view> every;
	for (const SettingRule& row : rows) {
		if (row.required) required.push_back(row.key);
		every.push_back(row.key);
	}
	std::string refusal = "settings: must be an object";
	if (!required.empty()) return refusal + " with " + quotedList(required);
	if (!every.empty()) return refusal + " with optional " + quotedList(every);
	return refusal;
}

/// Compared as JSON keeps it: an integer above INT64_MAX is unsigned, never wrapped negative, and nothing is cut to 32
/// bits first.
bool isWholeNumberWithin(const NodeSettings& value, std::uint32_t low, std::uint32_t high) {
	if (value.is_number_unsigned()) {
		const auto number = value.get<std::uint64_t>();
		return number >= low && number <= high;
	}
	if (value.is_number_integer()) {
		const auto number = value.get<std::int64_t>();
		return std::cmp_greater_equal(number, low) && std::cmp_less_equal(number, high);
	}
	return false;
}

std::string notWithinRange(const SettingRule& row) {
	std::string refusal = std::string(row.key) + ": must be a whole number";
	if (!row.unit.empty()) refusal += " of " + std::string(row.unit);
	return refusal + ", " + std::to_string(row.low) + "-" + std::to_string(row.high);
}

/// The row of `key` that is of `kind`.
const SettingRule& rowOf(std::span<const SettingRule> rows, std::string_view key, SettingKind kind) {
	const auto row = std::ranges::find(rows, key, &SettingRule::key);
	if (row == rows.end() || row->kind != kind) {
		throw std::logic_error("the settings table has no such key of this kind: " + std::string(key));
	}
	return *row;
}

} // namespace

CheckedSettings::CheckedSettings(std::span<const SettingRule> rows, const NodeSettings& settings) noexcept
	: rows_(rows), settings_(&settings) {}

std::optional<std::string_view> CheckedSettings::text(std::string_view key) const {
	rowOf(rows_, key, SettingKind::text);
	const auto value = settings_->find(key);
	if (value == settings_->end()) return std::nullopt;
	return std::string_view(value->get_ref<const std::string&>());
}

std::optional<std::uint32_t> CheckedSettings::wholeNumber(std::string_view key) const {
	const SettingRule& row = rowOf(rows_, key, SettingKind::wholeNumber);
	const auto value = settings_->find(key);
	if (value == settings_->end()) return row.byDefault;
	return value->get<std::uint32_t>();
}

std::expected<CheckedSettings, std::string> checkSettings(std::string_view type, std::span<const SettingRule> rows,
                                                          const NodeSettings& settings) {
	if (!settings.is_object() && !(rows.empty() && settings.is_null())) return std::unexpected(notAnObject(rows));

	for (const auto& [key, value] : settings.items()) {
		if (std::ranges::find(rows, std::string_view(key), &SettingRule::key) == rows.end()) {
			if (!rows.empty()) return std::unexpected("unknown key " + key);
			return std::unexpected("unknown key " + key + " (" + std::string(type) + " takes no settings)");
		}
	}

	for (const SettingRule& row : rows) {
		const auto value = settings.find(row.key);
		if (value == settings.end()) {
			if (row.required) return std::unexpected(std::string(row.key) + ": missing");
			continue;
		}
		if (row.across != nullptr) {
			if (const auto refusal = row.across(settings)) return std::unexpected(std::string(*refusal));
		}
		if (row.kind == SettingKind::wholeNumber) {
			if (!isWholeNumberWithin(*value, row.low, row.high)) return std::unexpected(notWithinRange(row));
		} else if (!value->is_string() || !row.means(value->get_ref<const std::string&>())) {
			return std::unexpected(std::string(row.key) + ": must be " + std::string(row.mustBe));
		}
	}
	return CheckedSettings(rows, settings);
}

} // namespace ear
