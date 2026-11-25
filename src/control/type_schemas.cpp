// The node types' schemas on the wire (docs/control-port.md#type-schemas). Each row says what the control port takes
// for its key (a rate is a whole number of Hz there; a boolean a JSON flag), never how ear.conf writes it, and each
// member a control needs to draw and hint the field: the editor's settings form is made from this alone.
#include "ear/control/type_schemas.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <utility>
#include <variant>
#include <vector>

#include "ear/control/commands.hpp"
#include "ear/control/graph_json.hpp"
#include "ear/core/settings_table.hpp"

namespace ear::control {
namespace {

using nlohmann::json;

std::string_view defaultFromName(setting::DefaultFrom from) noexcept {
	switch (from) {
	case setting::DefaultFrom::graphFrequency:
		return "graphFrequency";
	case setting::DefaultFrom::nodeName:
		return "nodeName";
	}
	return "nodeName";
}

json encodeRow(const SettingRow& row) {
	json out = {{"key", row.key},
	            {"required", row.required},
	            // From the row's marks (docs/architecture.md#restarts-and-changes-of-settings).
	            {"change", row.inPlace ? "inPlace" : "restart"}};
	std::visit(
		[&](const auto& rule) {
			using Rule = std::decay_t<decltype(rule)>;
			if constexpr (std::is_same_v<Rule, WholeNumberRule>) {
				out["kind"] = "wholeNumber";
				out["min"] = rule.low;
				out["max"] = rule.high;
				if (!rule.unit.empty()) out["unit"] = rule.unit;
			} else if constexpr (std::is_same_v<Rule, RateRule>) {
				// A whole number of hertz on the control port (docs/control-port.md#type-schemas).
				out["kind"] = "wholeNumber";
				out["min"] = rule.low.hz();
				out["max"] = rule.high.hz();
				out["unit"] = "Hz";
			} else if constexpr (std::is_same_v<Rule, BooleanRule>) {
				out["kind"] = "flag";
			} else {
				out["kind"] = "text";
				out["mustBe"] = rule.mustBe;
				if (!rule.hints.values.empty()) {
					json values = json::array();
					for (const std::string_view value : rule.hints.values) values.push_back(value);
					out["values"] = std::move(values);
				}
				if (!rule.hints.pattern.empty()) out["pattern"] = rule.hints.pattern;
				if (rule.hints.maxLength > 0) out["maxLength"] = rule.hints.maxLength;
			}
		},
		row.rule);
	if (row.byDefault) out["default"] = *row.byDefault;
	if (row.defaultFrom) out["defaultFrom"] = defaultFromName(*row.defaultFrom);
	if (!row.onlyWithout.empty()) out["onlyWithout"] = row.onlyWithout;
	if (!row.help.empty()) out["help"] = row.help;
	if (row.shapesPorts) out["shapes"] = true;
	return out;
}

} // namespace

json encodeTypeSchema(const TypeSchema& schema) {
	json settings = json::array();
	for (const SettingRow* row : schema.settings) settings.push_back(encodeRow(*row));
	json inputs = json::array();
	json outputs = json::array();
	for (const BankDeclaration& bank : schema.banks) {
		if (bank.reach != PortBank::Reach::routable) continue; // inside banks are never on the wire
		(bank.direction == Direction::input ? inputs : outputs).push_back(bank.name);
	}
	return {{"name", schema.name},
	        {"settings", std::move(settings)},
	        {"inputs", std::move(inputs)},
	        {"outputs", std::move(outputs)},
	        {"reconfigureMs", schema.reconfigureDeadline.count()}};
}

TypeSchemas encodeTypeSchemas(std::span<const TypeSchema> schemas) {
	TypeSchemas encoded;
	for (const TypeSchema& schema : schemas) encoded.typeSchemas.push_back(encodeTypeSchema(schema));
	encoded.hash = fnv1a64(encoded.typeSchemas.dump());
	return encoded;
}

TypeSchemas encodeTypeSchemas(const NodeCatalogue& catalogue) {
	std::vector<TypeSchema> schemas;
	for (const std::string_view name : catalogue.names()) schemas.push_back(catalogue.find(name)->schema());
	return encodeTypeSchemas(schemas);
}

std::string fnv1a64(std::string_view bytes) {
	std::uint64_t hash = 0xcbf29ce484222325ULL;
	for (const char byte : bytes) {
		hash ^= static_cast<unsigned char>(byte);
		hash *= 0x100000001b3ULL;
	}
	return std::format("fnv1a64:{:016x}", hash);
}

std::expected<std::optional<std::string>, RpcError> decodeGetTypes(const json& params) {
	const JsonPointer root;
	const json empty = json::object();
	const json& named = params.is_null() ? empty : params; // null: params absent
	const auto refuse = [](const JsonError& error) {
		return std::unexpected(RpcError{invalidParams, error.detail, {{"pointer", error.pointer}}});
	};
	if (auto checked = requireObject(named, root, {"ifNoneMatch"}); !checked) return refuse(checked.error());
	const auto given = named.find("ifNoneMatch");
	if (given == named.end()) return std::optional<std::string>{};
	if (!given->is_string()) return refuse(JsonError{"/ifNoneMatch: must be a string", "/ifNoneMatch"});
	return std::optional<std::string>(given->get<std::string>());
}

json typesResult(const TypeSchemas& schemas, const std::optional<std::string>& ifNoneMatch) {
	if (ifNoneMatch == schemas.hash) return {{"protocol", wireProtocol}, {"hash", schemas.hash}, {"notModified", true}};
	return {{"protocol", wireProtocol}, {"hash", schemas.hash}, {"typeSchemas", schemas.typeSchemas}};
}

} // namespace ear::control
