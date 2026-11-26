#include "ear/core/settings_table.hpp"

#include "caught.hpp"
#include "ear/core/decimal_text.hpp"
#include "ear/core/rate_text.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
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

std::string notWithinRange(std::string_view key, const DecimalRule& rule) {
	std::string refusal = std::string(key) + ": must be a number";
	if (!rule.unit.empty()) refusal += " of " + std::string(rule.unit);
	return refusal + " from " + decimalText(rule.low) + " to " + decimalText(rule.high);
}

/// One value read by `rule` for `key` (a plain row's, or a family member's full key): what the slot holds, or the
/// refusal's text.
std::expected<std::any, std::string> readValue(const SettingRule& rule, std::string_view key,
                                               const NodeSettings& value) {
	if (const auto* number = std::get_if<WholeNumberRule>(&rule)) {
		if (!isWholeNumberWithin(value, number->low, number->high))
			return std::unexpected(notWithinRange(key, *number));
		return value.get<std::uint32_t>();
	}
	if (const auto* rate = std::get_if<RateRule>(&rule)) {
		const auto read = rateOfSetting(value);
		if (!read || *read < rate->low || *read > rate->high) {
			return std::unexpected(std::string(key) + ": must be a rate from " +
			                       rangeText(RateRange{rate->low, rate->high}) +
			                       " (on the control port, a whole number of Hz)");
		}
		return *read;
	}
	if (const auto* decimal = std::get_if<DecimalRule>(&rule)) {
		// Any JSON number, an integer too; never a boolean, which nlohmann does not count as one.
		const bool taken = value.is_number() && std::isfinite(value.get<double>()) &&
		                   value.get<double>() >= decimal->low && value.get<double>() <= decimal->high;
		if (!taken) return std::unexpected(notWithinRange(key, *decimal));
		return value.get<double>();
	}
	if (std::holds_alternative<BooleanRule>(rule)) {
		if (!value.is_boolean()) return std::unexpected(std::string(key) + ": must be true or false");
		return value.get<bool>();
	}
	if (const auto* choice = std::get_if<ChoiceRule>(&rule)) {
		// A name exactly as declared: no case folding, no trimming, never a number for its index.
		if (value.is_string()) {
			const std::string& given = value.get_ref<const std::string&>();
			for (std::uint32_t at = 0; at < choice->count; ++at)
				if (choice->names[at] == given) return ChoiceIndex{at};
		}
		const std::span<const std::string_view> names = choice->taken();
		return std::unexpected(std::string(key) + ": must be one of " +
		                       quotedList(std::vector<std::string_view>(names.begin(), names.end())));
	}
	const TextRule& text = std::get<TextRule>(rule);
	std::any read;
	if (value.is_string()) read = text.read(value.get_ref<const std::string&>());
	if (!read.has_value()) return std::unexpected(std::string(key) + ": must be " + std::string(text.mustBe));
	return read;
}

/// A family's slot: its given members in number order, as FamilyValues of the rule's value.
template <class V> std::any familySlot(const std::vector<std::pair<std::uint32_t, std::any>>& members) {
	FamilyValues<V> values;
	values.reserve(members.size());
	for (const auto& [number, value] : members) values.emplace_back(number, std::any_cast<V>(value));
	return values;
}

} // namespace

std::expected<CheckedSettings, AnySettingsTable::Refusal> AnySettingsTable::walk(const NodeSettings& settings) const {
	if (!settings.is_object() && !(rows_.empty() && settings.is_null()))
		return std::unexpected(Refusal{notAnObject(rows_), {}});

	for (const auto& [key, value] : settings.items()) {
		if (rowOf(key)) continue;
		// A family member's spelling above the family's maximum is no member, and says why.
		for (const SettingRow* row : rows_) {
			if (row->maxMembers == 0 || !setting::memberNumber(key, row->key)) continue;
			return std::unexpected(Refusal{key + ": no such member (" + std::string(row->indexedBy) + " is at most " +
			                                   std::to_string(row->maxMembers) + ")",
			                               key});
		}
		if (!rows_.empty()) return std::unexpected(Refusal{"unknown key " + key, key});
		return std::unexpected(Refusal{"unknown key " + key + " (" + std::string(name_) + " takes no settings)", key});
	}

	std::vector<std::any> slots;
	slots.reserve(rows_.size());
	for (const SettingRow* row : rows_) {
		std::any& slot = slots.emplace_back();
		if (row->maxMembers > 0) {
			// A family walks in its place, member by member in number order; each refusal names the member's key.
			std::vector<std::pair<std::uint32_t, std::string_view>> given;
			for (const auto& [key, value] : settings.items()) {
				const auto member = setting::memberNumber(key, row->key);
				if (member && *member <= row->maxMembers) given.emplace_back(*member, key);
			}
			std::ranges::sort(given);
			std::vector<std::pair<std::uint32_t, std::any>> members;
			members.reserve(given.size());
			for (const auto& [number, key] : given) {
				auto read = readValue(row->rule, key, settings.at(std::string(key)));
				if (!read) return std::unexpected(Refusal{std::move(read.error()), std::string(key)});
				members.emplace_back(number, std::move(*read));
			}
			if (std::holds_alternative<WholeNumberRule>(row->rule)) slot = familySlot<std::uint32_t>(members);
			else if (std::holds_alternative<DecimalRule>(row->rule)) slot = familySlot<double>(members);
			else if (std::holds_alternative<ChoiceRule>(row->rule)) slot = familySlot<ChoiceIndex>(members);
			else slot = familySlot<bool>(members);
			continue;
		}
		// Every refusal from here on names this row's key.
		const auto refuse = [row](std::string text) {
			return std::unexpected(Refusal{std::move(text), std::string(row->key)});
		};
		const auto value = settings.find(row->key);
		if (value == settings.end()) {
			if (row->required) return refuse(std::string(row->key) + ": missing");
			if (row->byDefault) std::visit([&slot](const auto value) { slot = value; }, *row->byDefault);
			continue;
		}
		if (!row->onlyWithout.empty() && settings.contains(row->onlyWithout)) {
			return refuse(std::string(row->key) + R"(: only without ")" + std::string(row->onlyWithout) + '"');
		}
		auto read = readValue(row->rule, row->key, *value);
		if (!read) return refuse(std::move(read.error()));
		slot = std::move(*read);
	}
	return CheckedSettings(std::move(slots));
}

NodeType::NodeType(const AnySettingsTable& table, std::span<const StoreDeclaration> stores,
                   std::chrono::milliseconds reconfigureDeadline, std::span<const BankDeclaration> banks,
                   std::span<const StatusDeclaration* const> status, std::span<const EventDeclaration> events,
                   std::span<const ActionDeclaration> actions) noexcept
	: table_(&table), stores_(stores), reconfigureDeadline_(reconfigureDeadline), banks_(banks), status_(status),
	  events_(events), actions_(actions) {}

TypeSchema NodeType::schema() const noexcept {
	return TypeSchema{table_->name(), table_->rows(), banks_, reconfigureDeadline_, status_, events_, actions_};
}

std::expected<void, std::string> NodeConfig::runCall(std::size_t action, void*, CheckedSettings&&) const {
	throw std::logic_error("node config: action " + std::to_string(action) + " is no call of its type");
}

std::string_view NodeType::name() const noexcept { return table_->name(); }

std::span<const SettingDeclaration> NodeType::settingKinds() const noexcept { return table_->kinds(); }

std::optional<SettingKind> NodeType::kindOf(std::string_view key) const noexcept { return table_->kindOf(key); }

setting::Change NodeType::changeOf(std::string_view key) const noexcept { return table_->changeOf(key); }
std::optional<std::uint32_t> NodeType::liveIndexOf(std::string_view key) const noexcept {
	return table_->liveIndexOf(key);
}

std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings> NodeType::parse(const NodeName& node,
                                                                                  const NodeSettings& settings) const {
	// The type's own code (a text row's parse, its check, its ports) runs here, on the main loop
	// as an edit is planned: what it throws refuses the edit as an invalid setting would, never reaching the callback
	// boundary (docs/writing-a-node-type.md#rules-the-framework-enforces).
	std::string refusedKey;
	auto made = detail::caughtFrom([&]() -> MadeConfig {
		auto walked = table_->walk(settings);
		if (!walked) {
			refusedKey = std::move(walked.error().key);
			return std::unexpected(std::move(walked.error().text));
		}
		return makeConfig(node, std::move(*walked));
	});
	if (!made) return std::unexpected(InvalidSettings{node, std::string(name()) + " threw: " + made.error()});
	// A refusal of the walk names its key; the type's check and make name none (docs/control-port.md#error-codes).
	if (!*made) return std::unexpected(InvalidSettings{node, std::move(made->error()), std::move(refusedKey)});
	return std::move(**made);
}

} // namespace ear
