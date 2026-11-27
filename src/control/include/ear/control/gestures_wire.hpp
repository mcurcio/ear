// Gestures on the wire (feature gestures, docs/control-port.md#gestures): the four requests as typed commands, what
// the main loop answers them with, and gesture.changed. Control thread only, but the plain values, which cross.
#pragma once
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>
#include "ear/control/rpc.hpp"
#include "ear/control/commands.hpp"
#include "ear/core/gestures.hpp"
namespace ear::control {
/// gesture.begin {"name", "key"}.
struct BeginGesture {
	NodeName node;
	std::string key;
};
/// gesture.change {"gesture", "seq", "value"}: a notification only.
struct MoveGesture {
	std::string gesture;
	std::uint64_t seq;
	double value;
};
/// gesture.end {"gesture", "value"?}.
struct EndGesture {
	std::string gesture;
	std::optional<double> value;
};
/// gesture.cancel {"gesture"}.
struct CancelGesture {
	std::string gesture;
};
using GestureCommand = std::variant<BeginGesture, MoveGesture, EndGesture, CancelGesture>;
/// Whether `method` is one of the four (gesture.begin, gesture.change, gesture.end, gesture.cancel).
bool isGestureMethod(std::string_view method) noexcept;
/// Control thread. A gesture method's params, named only, as decodeCommand decodes the others: -32602 with
/// data.pointer for a missing, extra or mistyped member; a value must be a finite number, a seq a whole number from 1.
std::expected<GestureCommand, RpcError> decodeGesture(std::string_view method, const nlohmann::json& params);
/// A begin's answer: the gesture's id, the committed value, its reach and the range and decimals it moves in.
struct GestureBegun {
	std::string gesture;
	double value;
	std::vector<GestureKey> reach;
	double min;
	double max;
	unsigned decimals;
};
struct GestureCancelled {};
/// A gesture request refused by the main loop before any keeper step: its data.code, message and the code's own data
/// members (docs/control-port.md#error-codes).
struct GestureRefused {
	std::string code;
	std::string message;
	nlohmann::json data = nlohmann::json::object();
};
/// The data.code of the refusals only gesture requests get.
inline constexpr std::string_view notLiveCode = "not-live";
inline constexpr std::string_view reachMismatchCode = "reach-mismatch";
inline constexpr std::string_view fileRefusedCode = "file-refused";
inline constexpr std::string_view unknownGestureCode = "unknown-gesture";
inline constexpr std::string_view notInBatchCode = "not-in-batch";
inline constexpr std::string_view notificationOnlyCode = "notification-only";
/// What a gesture request is answered with: its own result, a refusal before the keeper, or (an end's) the keeper's
/// refusal of the commit.
using GestureAnswer = std::variant<GestureBegun, GestureEnded, GestureCancelled, GestureRefused, CommandResult>;
/// {"gesture", "value", "reach", "min", "max", "decimals"}, {"value"}, {} or the error.
std::expected<nlohmann::json, RpcError> encodeGestureAnswer(const GestureAnswer& answer);
/// One gesture.changed notification (docs/control-port.md#gestures).
std::string encodeGestureChanged(const GestureNote& note);
/// gesture.changed's `phase`: begin, change, end, aborted, tick-refused.
std::string_view gesturePhaseName(GesturePhase phase) noexcept;
} // namespace ear::control
