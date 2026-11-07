#include "ear/core/log.hpp"

namespace ear {

std::string_view logLevelName(LogLevel level) noexcept {
	switch (level) {
	case LogLevel::debug:
		return "debug";
	case LogLevel::info:
		return "info";
	case LogLevel::warn:
		return "warn";
	case LogLevel::error:
		return "error";
	}
	return "info";
}

} // namespace ear
