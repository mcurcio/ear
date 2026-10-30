// The control calls' error codes and their one-line descriptions (docs/design/m1-white-to-file.md
// section 2, command_error.hpp): the one home of the code strings.
#include "ear/core/command_error.hpp"

#include <format>
#include <type_traits>

namespace ear {
namespace {

std::string portText(const PortRef& port) {
	return std::format("{}:{}", port.node, port.port);
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
			else {
				static_assert(std::is_same_v<E, WrongDirection>);
				return "wrong-direction";
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
			else if constexpr (std::is_same_v<E, DuplicateNode> || std::is_same_v<E, UnknownNode>)
				return std::format("{} {}", code, e.node);
			else if constexpr (std::is_same_v<E, UnknownPort>)
				return std::format("{} {}", code, portText(e.port));
			else
				return std::format("{} {} -> {}", code, portText(e.route.from), portText(e.route.to));
		},
		error);
}

} // namespace ear
