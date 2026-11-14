// The one catch every call into a type's code goes through (docs/writing-a-node-type.md#rules-the-framework-enforces):
// NodeHost's hooks and consumers, and a type's parse (NodeType::parse). Private to ear_core.
#pragma once
#include <exception>
#include <expected>
#include <string>
#include <type_traits>
#include <utility>

namespace ear::detail {

/// What a caught exception says, for a detail.
inline std::string whatOf(const std::exception_ptr& thrown) {
	try {
		std::rethrow_exception(thrown);
	} catch (const std::exception& error) {
		return error.what();
	} catch (...) {
		return "an exception that is no std::exception";
	}
}

/// Runs a type's code and says what it threw instead of letting it pass, on whatever thread calls it, so a type's
/// throw is a fact about its node (or a refused edit, at parse) and never reaches the callback boundary.
template <typename Call> auto caughtFrom(Call&& call) -> std::expected<std::invoke_result_t<Call>, std::string> {
	try {
		if constexpr (std::is_void_v<std::invoke_result_t<Call>>) {
			std::forward<Call>(call)();
			return {};
		} else {
			return std::forward<Call>(call)();
		}
	} catch (...) {
		return std::unexpected(whatOf(std::current_exception()));
	}
}

} // namespace ear::detail
