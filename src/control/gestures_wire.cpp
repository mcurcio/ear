// Gestures on the wire (gestures_wire.hpp; docs/control-port.md#gestures).
#include "ear/control/gestures_wire.hpp"

#include <cmath>
#include <utility>

#include "ear/control/graph_json.hpp"

namespace ear::control {
namespace {

using nlohmann::json;

std::unexpected<RpcError> refuse(const JsonError& error) {
	return std::unexpected(RpcError{invalidParams, error.detail, {{"pointer", error.pointer}}});
}

JsonError mistyped(const JsonPointer& at, const std::string& key, std::string_view must) {
	return JsonError{(at / key).to_string() + ": must be " + std::string(must), (at / key).to_string()};
}

/// A gesture id: a non-empty string.
std::expected<std::string, JsonError> requireId(const json& params, const JsonPointer& at) {
	return requireName(params, at, "gesture");
}

std::expected<double, JsonError> requireValue(const json& params, const JsonPointer& at) {
	auto member = requireMember(params, at, "value");
	if (!member) return std::unexpected(member.error());
	if (!(*member)->is_number() || !std::isfinite((*member)->get<double>()))
		return std::unexpected(mistyped(at, "value", "a finite number"));
	return (*member)->get<double>();
}

json encodeKeys(const std::vector<GestureKey>& keys) {
	json all = json::array();
	for (const GestureKey& at : keys) all.push_back({{"name", at.node}, {"key", at.key}});
	return all;
}

} // namespace

bool isGestureMethod(std::string_view method) noexcept {
	return method == "gesture.begin" || method == "gesture.change" || method == "gesture.end" ||
	       method == "gesture.cancel";
}

std::expected<GestureCommand, RpcError> decodeGesture(std::string_view method, const json& params) {
	const JsonPointer at;
	if (params.is_array()) return refuse(requireObject(params, at, {}).error());
	const json empty = json::object();
	const json& named = params.is_null() ? empty : params;
	if (method == "gesture.begin") {
		if (auto checked = requireObject(named, at, {"name", "key"}); !checked) return refuse(checked.error());
		auto name = requireName(named, at, "name");
		if (!name) return refuse(name.error());
		auto key = requireName(named, at, "key");
		if (!key) return refuse(key.error());
		return BeginGesture{std::move(*name), std::move(*key)};
	}
	if (method == "gesture.change") {
		if (auto checked = requireObject(named, at, {"gesture", "seq", "value"}); !checked)
			return refuse(checked.error());
		auto id = requireId(named, at);
		if (!id) return refuse(id.error());
		auto seq = requireMember(named, at, "seq");
		if (!seq) return refuse(seq.error());
		if (!(*seq)->is_number_unsigned() || (*seq)->get<std::uint64_t>() == 0)
			return refuse(mistyped(at, "seq", "a whole number from 1"));
		auto value = requireValue(named, at);
		if (!value) return refuse(value.error());
		return MoveGesture{std::move(*id), (*seq)->get<std::uint64_t>(), *value};
	}
	if (method == "gesture.end") {
		if (auto checked = requireObject(named, at, {"gesture", "value"}); !checked) return refuse(checked.error());
		auto id = requireId(named, at);
		if (!id) return refuse(id.error());
		std::optional<double> value;
		if (named.contains("value")) {
			auto given = requireValue(named, at);
			if (!given) return refuse(given.error());
			value = *given;
		}
		return EndGesture{std::move(*id), value};
	}
	if (auto checked = requireObject(named, at, {"gesture"}); !checked) return refuse(checked.error());
	auto id = requireId(named, at);
	if (!id) return refuse(id.error());
	return CancelGesture{std::move(*id)};
}

std::expected<json, RpcError> encodeGestureAnswer(const GestureAnswer& answer) {
	if (const auto* begun = std::get_if<GestureBegun>(&answer))
		return json{{"gesture", begun->gesture}, {"value", begun->value}, {"reach", encodeKeys(begun->reach)},
		            {"min", begun->min},         {"max", begun->max},     {"decimals", begun->decimals}};
	if (std::holds_alternative<GestureCancelled>(answer)) return json::object();
	if (const auto* refused = std::get_if<GestureRefused>(&answer)) {
		json data = refused->data;
		data["code"] = refused->code;
		return std::unexpected(RpcError{commandRefused, refused->message, std::move(data)});
	}
	return encodeCommandResult(std::get<CommandResult>(answer));
}

std::string_view gesturePhaseName(GesturePhase phase) noexcept {
	switch (phase) {
	case GesturePhase::begin:
		return "begin";
	case GesturePhase::change:
		return "change";
	case GesturePhase::end:
		return "end";
	case GesturePhase::aborted:
		return "aborted";
	case GesturePhase::tickRefused:
		return "tick-refused";
	}
	return "change";
}

std::string encodeGestureChanged(const GestureNote& note) {
	json params = {{"gesture", note.gesture}, {"phase", gesturePhaseName(note.phase)}};
	switch (note.phase) {
	case GesturePhase::begin:
		params["value"] = note.value.value_or(0.0);
		params["reach"] = encodeKeys(note.reach);
		break;
	case GesturePhase::change:
		params["value"] = note.value.value_or(0.0);
		break;
	case GesturePhase::end:
		params["value"] = note.value.value_or(0.0);
		if (!note.reason.empty()) params["reason"] = note.reason;
		break;
	case GesturePhase::aborted:
		params["reason"] = note.reason;
		if (note.value) params["value"] = *note.value;
		break;
	case GesturePhase::tickRefused:
		params["code"] = note.code;
		params["key"] = note.key;
		// The refusal in its own words, as invalid-settings' data.detail (docs/control-port.md#error-codes).
		params["detail"] =
			note.detail.starts_with(note.key + ": ") ? note.detail.substr(note.key.size() + 2) : note.detail;
		break;
	}
	return json{{"jsonrpc", "2.0"}, {"method", "gesture.changed"}, {"params", std::move(params)}}.dump();
}

} // namespace ear::control
