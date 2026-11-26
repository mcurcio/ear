// Every node type together, however many the folders in src/nodes make: their names, and each type's declared setting
// kinds (docs/graph-file.md). That main.cpp registers them is the system tests' to prove.
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "all_types.hpp"
#include "catch2/catch.hpp"
#include "ear/core/node_catalogue.hpp"
#include "ear/core/settings_table.hpp"

// Each found; the whole list is the next test's to prove against the folders, and sendspin is the last test's.
TEST_CASE("The built-in node types are white, file, card and rtp", "[builtin]") {
	const ear::NodeCatalogue catalogue(ear::test::allTypes());
	for (const std::string_view name : {"white", "file", "card", "rtp"}) {
		INFO(name);
		CHECK(catalogue.find(name) != nullptr);
	}
}

TEST_CASE("Every folder in src/nodes makes one type of its own name", "[nodes]") {
	const ear::NodeCatalogue catalogue(ear::test::allTypes());
	std::string joined;
	for (const std::string_view name : catalogue.names()) {
		if (!joined.empty()) joined += ',';
		joined += name;
	}
	CHECK(joined == EAR_NODE_TYPE_FOLDERS);
}

TEST_CASE("Each built-in type declares every key once, and a value of each declared kind parses", "[builtin]") {
	std::map<std::string, std::vector<std::pair<std::string, ear::SettingKind>>> declared;
	for (const auto& type : ear::test::allTypes()) {
		INFO(type->name());
		const auto& valid = ear::test::validSettings(type->name());
		for (const ear::NodeSettings& settings : valid) {
			const auto parsed = type->parse("node", settings);
			INFO(settings.dump() << " is refused: " << (parsed ? "" : parsed.error().detail));
			CHECK(parsed.has_value());
		}
		auto& keys = declared[std::string(type->name())];
		for (const auto& [key, kind] : type->settingKinds()) {
			INFO(key);
			const std::string spelled(key);
			CHECK(std::ranges::none_of(keys, [&](const auto& seen) { return seen.first == spelled; }));
			const auto giving = std::ranges::find_if(valid, [&](const auto& one) { return one.contains(spelled); });
			REQUIRE(giving != valid.end());
			CHECK((kind == ear::SettingKind::text || kind == ear::SettingKind::choice) ==
			      giving->at(spelled).is_string());
			keys.emplace_back(key, kind);
		}
	}
	using enum ear::SettingKind;
	CHECK(declared["white"].empty());
	CHECK(declared["airplay"] ==
	      std::vector<std::pair<std::string, ear::SettingKind>>{{"name", text}, {"airplay2", boolean}});
	CHECK(declared["file"] == std::vector<std::pair<std::string, ear::SettingKind>>{{"path", text}});
	CHECK(declared["card"] == std::vector<std::pair<std::string, ear::SettingKind>>{
								  {"card", text}, {"playback", wholeNumber}, {"capture", wholeNumber}});
	CHECK(declared["rtp"] == std::vector<std::pair<std::string, ear::SettingKind>>{{"address", text},
	                                                                               {"port", wholeNumber},
	                                                                               {"format", text},
	                                                                               {"channels", wholeNumber},
	                                                                               {"rate", rate},
	                                                                               {"latencyMs", wholeNumber}});
	CHECK(declared["sendspin"] == std::vector<std::pair<std::string, ear::SettingKind>>{
									  {"server", text}, {"port", wholeNumber}, {"name", text}});
}

namespace {

/// The public ports `type` makes of `settings`; none when it refuses them.
std::optional<ear::Ports> portsOf(const ear::NodeType& type, const ear::NodeSettings& settings) {
	const auto parsed = type.parse("node", settings);
	if (!parsed) return std::nullopt;
	return (*parsed)->ports().publicOnly();
}

/// Other values a row can take, to change one key of valid settings at a time: a whole number's ends, middle and
/// neighbours, a rate's ends, a decimal's ends and middle, a boolean's other value, a choice's every name, a text row's
/// offered values, the value every other valid object gives it, and absence for a row that is not required.
std::vector<std::optional<ear::NodeSettings>> otherValues(const ear::SettingRow& row,
                                                          const std::vector<ear::NodeSettings>& valid) {
	std::vector<std::optional<ear::NodeSettings>> values;
	if (const auto* number = std::get_if<ear::WholeNumberRule>(&row.rule)) {
		for (const std::uint64_t value :
		     {std::uint64_t{number->low}, std::uint64_t{number->high}, (std::uint64_t{number->low} + number->high) / 2,
		      std::uint64_t{number->low} + 1})
			if (value <= number->high) values.emplace_back(value);
	} else if (const auto* rate = std::get_if<ear::RateRule>(&row.rule)) {
		values.emplace_back(rate->low.hz());
		values.emplace_back(rate->high.hz());
	} else if (const auto* decimal = std::get_if<ear::DecimalRule>(&row.rule)) {
		values.emplace_back(decimal->low);
		values.emplace_back(decimal->high);
		values.emplace_back((decimal->low + decimal->high) / 2);
	} else if (std::holds_alternative<ear::BooleanRule>(row.rule)) {
		values.emplace_back(true);
		values.emplace_back(false);
	} else if (const auto* choice = std::get_if<ear::ChoiceRule>(&row.rule)) {
		for (const std::string_view name : choice->taken()) values.emplace_back(std::string(name));
	} else {
		for (const std::string_view value : std::get<ear::TextRule>(row.rule).hints.values)
			values.emplace_back(std::string(value));
	}
	for (const ear::NodeSettings& one : valid)
		if (const auto given = one.find(row.key); given != one.end()) values.emplace_back(*given);
	if (!row.required) values.emplace_back(std::nullopt);
	return values;
}

} // namespace

// The type schema's marks agree with what the type does (docs/writing-a-node-type.md#the-type-schema): a row that changes the
// ports for any value tried is marked shapesPorts, and every row so marked changes them for one; the routable banks are
// those ports() makes; a text row's hints hold for every value its parse takes here.
TEST_CASE("Each built-in type's schema holds to its ports and its parse", "[builtin][schema]") {
	for (const auto& type : ear::test::allTypes()) {
		INFO(type->name());
		const ear::TypeSchema schema = type->schema();
		CHECK(schema.name == type->name());
		// Every status field starts at a value of its kind, or unknown (docs/writing-a-node-type.md#status-fields).
		for (const ear::StatusDeclaration* field : schema.status) {
			INFO(field->name);
			const ear::StatusValue initial = field->initial();
			switch (field->kind) {
			case ear::ValueKind::flag:
				CHECK(std::holds_alternative<bool>(initial));
				break;
			case ear::ValueKind::wholeNumber:
				CHECK(std::holds_alternative<std::int64_t>(initial));
				break;
			case ear::ValueKind::decimal:
				CHECK(std::holds_alternative<double>(initial));
				break;
			case ear::ValueKind::choice:
				CHECK((std::holds_alternative<std::monostate>(initial) ||
				       std::ranges::find(field->values, std::get<std::string>(initial)) != field->values.end()));
				continue;
			case ear::ValueKind::text:
				CHECK(std::holds_alternative<std::string>(initial));
				break;
			}
			if (field->initialUnknown) CHECK(std::holds_alternative<std::monostate>(initial));
		}
		REQUIRE(schema.settings.size() == type->settingKinds().size());
		const auto& valid = ear::test::validSettings(type->name());
		for (const ear::NodeSettings& settings : valid) {
			INFO(settings.dump());
			const auto ports = portsOf(*type, settings);
			REQUIRE(ports.has_value());
			std::vector<std::string_view> routable;
			for (const ear::BankDeclaration& bank : schema.banks)
				if (bank.reach == ear::PortBank::Reach::routable) routable.push_back(bank.name);
			std::vector<std::string_view> made;
			for (const auto& bank : ports->banks()) made.push_back(bank.name);
			CHECK(made == routable);
		}
		for (std::size_t at = 0; at < schema.settings.size(); ++at) {
			const ear::SettingRow& row = *schema.settings[at];
			INFO(row.key);
			CHECK(row.key == type->settingKinds()[at].key);
			const std::string key(row.key);
			bool changedPorts = false;
			for (const ear::NodeSettings& settings : valid) {
				const auto before = portsOf(*type, settings);
				for (const auto& value : otherValues(row, valid)) {
					ear::NodeSettings changed = settings;
					if (value) changed[key] = *value;
					else changed.erase(key);
					const auto after = portsOf(*type, changed);
					INFO(changed.dump());
					if (after && *after != *before) {
						changedPorts = true;
						CHECK(row.shapesPorts);
					}
				}
			}
			if (row.shapesPorts) CHECK(changedPorts);
			// A control must be able to say what an absent optional row means.
			if (!row.required && !row.byDefault) CHECK((row.defaultFrom.has_value() || !row.help.empty()));
			if (const auto* text = std::get_if<ear::TextRule>(&row.rule)) {
				const ear::setting::TextHints& hints = text->hints;
				for (const std::string_view value : hints.values) {
					INFO(value);
					CHECK(text->read(value).has_value());
				}
				for (const ear::NodeSettings& settings : valid) {
					const auto given = settings.find(key);
					if (given == settings.end()) continue;
					const auto& value = given->get_ref<const std::string&>();
					INFO(value);
					if (!hints.values.empty()) CHECK(std::ranges::find(hints.values, value) != hints.values.end());
					if (!hints.pattern.empty())
						CHECK(std::regex_search(value, std::regex(std::string(hints.pattern), std::regex::ECMAScript)));
					if (hints.maxLength > 0) CHECK(value.size() <= hints.maxLength);
				}
			}
		}
	}
}
