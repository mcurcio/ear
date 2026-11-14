#include "ear/core/settings_table.hpp"

#include "caught.hpp"
#include "ear/core/rate_text.hpp"

#include <algorithm>
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

std::string notAnObject(std::span<const SettingRow* const> rows) {
	std::vector<std::string_view> required;
	std::vector<std::string_view> every;
	for (const SettingRow* row : rows) {
		if (row->required) required.push_back(row->key);
		every.push_back(row->key);
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

std::string notWithinRange(std::string_view key, const WholeNumberRule& rule) {
	std::string refusal = std::string(key) + ": must be a whole number";
	if (!rule.unit.empty()) refusal += " of " + std::string(rule.unit);
	return refusal + ", " + std::to_string(rule.low) + "-" + std::to_string(rule.high);
}

} // namespace

std::expected<CheckedSettings, std::string> AnySettingsTable::walk(const NodeSettings& settings) const {
	if (!settings.is_object() && !(rows_.empty() && settings.is_null())) return std::unexpected(notAnObject(rows_));

	for (const auto& [key, value] : settings.items()) {
		if (std::ranges::find(rows_, std::string_view(key), &SettingRow::key) == rows_.end()) {
			if (!rows_.empty()) return std::unexpected("unknown key " + key);
			return std::unexpected("unknown key " + key + " (" + std::string(name_) + " takes no settings)");
		}
	}

	std::vector<std::any> slots;
	slots.reserve(rows_.size());
	for (const SettingRow* row : rows_) {
		std::any& slot = slots.emplace_back();
		const auto* number = std::get_if<WholeNumberRule>(&row->rule);
		const auto value = settings.find(row->key);
		if (value == settings.end()) {
			if (row->required) return std::unexpected(std::string(row->key) + ": missing");
			if (row->byDefault) slot = *row->byDefault;
			continue;
		}
		if (!row->onlyWithout.empty() && settings.contains(row->onlyWithout)) {
			return std::unexpected(std::string(row->key) + R"(: only without ")" + std::string(row->onlyWithout) + '"');
		}
		if (number != nullptr) {
			if (!isWholeNumberWithin(*value, number->low, number->high)) {
				return std::unexpected(notWithinRange(row->key, *number));
			}
			slot = value->get<std::uint32_t>();
		} else if (const auto* rate = std::get_if<RateRule>(&row->rule)) {
			const auto read = rateOfSetting(*value);
			if (!read || *read < rate->low || *read > rate->high) {
				return std::unexpected(std::string(row->key) + ": must be a rate from " +
				                       rangeText(RateRange{rate->low, rate->high}) +
				                       " (on the control port, a whole number of Hz)");
			}
			slot = *read;
		} else {
			const TextRule& text = std::get<TextRule>(row->rule);
			if (value->is_string()) slot = text.read(value->get_ref<const std::string&>());
			if (!slot.has_value())
				return std::unexpected(std::string(row->key) + ": must be " + std::string(text.mustBe));
		}
	}
	return CheckedSettings(std::move(slots));
}

NodeType::NodeType(const AnySettingsTable& table, std::span<const StoreDeclaration> stores) noexcept
	: table_(&table), stores_(stores) {}

std::string_view NodeType::name() const noexcept { return table_->name(); }

std::span<const SettingDeclaration> NodeType::settingKinds() const noexcept { return table_->kinds(); }

std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings> NodeType::parse(const NodeName& node,
                                                                                  const NodeSettings& settings) const {
	// The type's own code (a text row's parse, its check, its ports, a legacy type's config) runs here, on the main loop
	// as an edit is planned: what it throws refuses the edit as an invalid setting would, never reaching the callback
	// boundary (docs/writing-a-node-type.md#rules-the-framework-enforces).
	auto made = detail::caughtFrom([&]() -> MadeConfig {
		auto walked = table_->walk(settings);
		if (!walked) return std::unexpected(std::move(walked.error()));
		return makeConfig(node, std::move(*walked));
	});
	if (!made) return std::unexpected(InvalidSettings{node, std::string(name()) + " threw: " + made.error()});
	if (!*made) return std::unexpected(InvalidSettings{node, std::move(made->error())});
	return std::move(**made);
}

} // namespace ear
