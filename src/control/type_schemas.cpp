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

std::string_view taperName(setting::Taper taper) noexcept {
	switch (taper) {
	case setting::Taper::linear:
		return "linear";
	case setting::Taper::log:
		return "log";
	case setting::Taper::db:
		return "db";
	}
	return "linear";
}

json encodeRow(const SettingRow& row) {
	json out = {{"key", row.key},
	            {"required", row.required},
	            // From the row's change mark (docs/writing-a-node-type.md#change-marks).
	            {"change", row.change == setting::Change::inPlace ? "inPlace" : "restart"}};
	if (!row.restartReason.empty()) out["restartReason"] = row.restartReason;
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
			} else if constexpr (std::is_same_v<Rule, DecimalRule>) {
				// Named as a decimal status field's are (docs/control-port.md#type-schemas).
				out["kind"] = "decimal";
				out["min"] = rule.low;
				out["max"] = rule.high;
				if (!rule.unit.empty()) out["unit"] = rule.unit;
				out["decimals"] = rule.hints.decimals;
				out["taper"] = taperName(rule.hints.taper);
				if (rule.hints.minIsSilence) out["minIsSilence"] = true;
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
	if (row.byDefault) std::visit([&out](const auto value) { out["default"] = value; }, *row.byDefault);
	if (row.defaultFrom) out["defaultFrom"] = defaultFromName(*row.defaultFrom);
	if (!row.onlyWithout.empty()) out["onlyWithout"] = row.onlyWithout;
	if (!row.help.empty()) out["help"] = row.help;
	if (row.shapesPorts) out["shapes"] = true;
	// A family: its members' keys are <key>.<n>, n from 1 to its count row's high (docs/control-port.md#type-schemas).
	if (row.maxMembers > 0) out["indexed"] = {{"by", row.indexedBy}, {"from", 1}, {"max", row.maxMembers}};
	return out;
}

} // namespace

/// A status field as types.get says it (docs/control-port.md#type-schemas).
json encodeStatusField(const StatusDeclaration& field) {
	json out = {
		{"name", field.name},
		{"kind", valueKindName(field.kind)},
		{"initial", nullptr},
		{"display", displayName(field.display)},
		{"flapsAbove", {{"transitions", field.flapsAbove.transitions}, {"windowMs", field.flapsAbove.windowMs}}}};
	const StatusValue initial = field.initial();
	if (const auto* flag = std::get_if<bool>(&initial)) out["initial"] = *flag;
	else if (const auto* number = std::get_if<std::int64_t>(&initial)) out["initial"] = *number;
	else if (const auto* decimal = std::get_if<double>(&initial)) out["initial"] = *decimal;
	else if (const auto* text = std::get_if<std::string>(&initial)) out["initial"] = *text;
	switch (field.kind) {
	case ValueKind::wholeNumber:
		out["min"] = field.low;
		out["max"] = field.high;
		break;
	case ValueKind::decimal:
		out["min"] = field.lowDecimal;
		out["max"] = field.highDecimal;
		out["decimals"] = field.decimals;
		break;
	case ValueKind::choice:
		out["values"] = json(std::vector<std::string_view>(field.values.begin(), field.values.end()));
		break;
	case ValueKind::text:
		out["maxBytes"] = field.maxBytes;
		break;
	case ValueKind::flag:
		break;
	}
	if (!field.unit.empty()) out["unit"] = field.unit;
	if (!field.help.empty()) out["help"] = field.help;
	return out;
}

/// An event as types.get says it: its code, severity, sentence and arguments.
json encodeEventDeclaration(const EventDeclaration& event) {
	json args = json::array();
	for (const ArgDeclaration& arg : event.args) {
		json out = {{"name", arg.name}, {"kind", valueKindName(arg.kind)}};
		if (arg.kind == ValueKind::wholeNumber) {
			out["min"] = arg.low;
			out["max"] = arg.high;
		}
		if (arg.kind == ValueKind::decimal) out["decimals"] = arg.decimals;
		if (arg.kind == ValueKind::choice)
			out["values"] = json(std::vector<std::string_view>(arg.values.begin(), arg.values.end()));
		if (arg.kind == ValueKind::text) out["maxBytes"] = arg.maxBytes;
		args.push_back(std::move(out));
	}
	return {{"code", event.code},
	        {"severity", severityName(event.severity)},
	        {"sentence", event.sentence},
	        {"args", std::move(args)}};
}

/// A type action as types.get says it (docs/control-port.md#actions): its arguments are setting rows, without what
/// only a setting has (`change`, `restartReason`, `shapes`).
json encodeAction(const ActionDeclaration& action) {
	json args = json::array();
	for (const SettingRow* row : action.args) {
		json out = encodeRow(*row);
		out.erase("change");
		out.erase("restartReason");
		out.erase("shapes");
		args.push_back(std::move(out));
	}
	return {{"name", action.name},
	        {"by", "type"},
	        {"target", action.target == ActionDeclaration::Target::code ? "code" : "loop"},
	        {"help", action.help},
	        {"args", std::move(args)}};
}

json encodeTypeSchema(const TypeSchema& schema) {
	json settings = json::array();
	for (const SettingRow* row : schema.settings) settings.push_back(encodeRow(*row));
	json inputs = json::array();
	json outputs = json::array();
	for (const BankDeclaration& bank : schema.banks) {
		if (bank.reach != PortBank::Reach::routable) continue; // inside banks are never on the wire
		(bank.direction == Direction::input ? inputs : outputs).push_back(bank.name);
	}
	json status = json::array();
	for (const StatusDeclaration* field : schema.status) status.push_back(encodeStatusField(*field));
	json events = json::array();
	for (const EventDeclaration& event : schema.events) events.push_back(encodeEventDeclaration(event));
	json actions = json::array();
	for (const ActionDeclaration& action : schema.actions) actions.push_back(encodeAction(action));
	return {{"name", schema.name},
	        {"settings", std::move(settings)},
	        {"inputs", std::move(inputs)},
	        {"outputs", std::move(outputs)},
	        {"reconfigureMs", schema.reconfigureDeadline.count()},
	        {"status", std::move(status)},
	        {"events", std::move(events)},
	        {"actions", std::move(actions)}};
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
