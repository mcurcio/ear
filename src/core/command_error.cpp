// The control calls' error codes and their one-line descriptions (docs/design/batch2.md
// section 4.1, command_error.hpp): the one home of the code strings.
#include "ear/core/command_error.hpp"

#include <format>
#include <type_traits>

namespace ear {
namespace {

std::string portText(const PortRef& port) {
	return std::format("{}:{}", port.node, port.port);
}

std::string routeText(const Route& route) {
	return std::format("{} -> {}", portText(route.from), portText(route.to));
}

} // namespace

std::string_view errorCode(const CommandError& error) noexcept {
	return std::visit(
		[](const auto& e) -> std::string_view {
			using E = std::decay_t<decltype(e)>;
			if constexpr (std::is_same_v<E, UnknownNodeType>) return "unknown-node-type";
			else if constexpr (std::is_same_v<E, InvalidSettings>) return "invalid-settings";
			else if constexpr (std::is_same_v<E, DuplicateNode>) return "duplicate-node";
			else if constexpr (std::is_same_v<E, UnknownNode>) return "unknown-node";
			else if constexpr (std::is_same_v<E, UnknownPort>) return "unknown-port";
			else if constexpr (std::is_same_v<E, WrongDirection>) return "wrong-direction";
			else if constexpr (std::is_same_v<E, InvalidName>) return "invalid-name";
			else if constexpr (std::is_same_v<E, PortsChanged>) return "ports-changed";
			else if constexpr (std::is_same_v<E, UnknownRoute>) return "unknown-route";
			else {
				static_assert(std::is_same_v<E, Stopping>);
				return "stopping";
			}
		},
		error);
}

std::string describe(const CommandError& error) {
	const std::string_view code = errorCode(error);
	return std::visit(
		[code](const auto& e) -> std::string {
			using E = std::decay_t<decltype(e)>;
			if constexpr (std::is_same_v<E, UnknownNodeType>) return std::format("{} {}", code, e.type);
			else if constexpr (std::is_same_v<E, InvalidSettings>)
				return std::format("{} {}: {}", code, e.node, e.detail);
			else if constexpr (std::is_same_v<E, DuplicateNode> || std::is_same_v<E, UnknownNode> ||
			                   std::is_same_v<E, InvalidName> || std::is_same_v<E, PortsChanged>)
				return std::format("{} {}", code, e.node);
			else if constexpr (std::is_same_v<E, UnknownPort>)
				return std::format("{} {}", code, portText(e.port));
			else if constexpr (std::is_same_v<E, WrongDirection> || std::is_same_v<E, UnknownRoute>)
				return std::format("{} {}", code, routeText(e.route));
			else {
				static_assert(std::is_same_v<E, Stopping>);
				return std::string(code);
			}
		},
		error);
}

} // namespace ear
