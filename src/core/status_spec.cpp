#include "ear/core/status_declarations.hpp"
namespace ear {
StatusValue StatusDeclaration::initial() const {
	if (initialUnknown) return std::monostate{};
	switch (kind) {
	case ValueKind::flag:
		return initialFlag;
	case ValueKind::wholeNumber:
		return initialNumber;
	case ValueKind::decimal:
		return initialDecimal;
	case ValueKind::choice:
		return std::string(values[initialChoice]);
	case ValueKind::text:
		return std::string(initialText);
	}
	return std::monostate{};
}

std::string_view displayName(Display display) noexcept {
	switch (display) {
	case Display::panel:
		return "panel";
	case Display::box:
		return "box";
	case Display::callout:
		return "callout";
	}
	return "panel";
}

std::string_view severityName(EventSeverity severity) noexcept {
	switch (severity) {
	case EventSeverity::info:
		return "info";
	case EventSeverity::warn:
		return "warn";
	case EventSeverity::error:
		return "error";
	}
	return "info";
}

std::string_view valueKindName(ValueKind kind) noexcept {
	switch (kind) {
	case ValueKind::flag:
		return "flag";
	case ValueKind::wholeNumber:
		return "wholeNumber";
	case ValueKind::decimal:
		return "decimal";
	case ValueKind::choice:
		return "choice";
	case ValueKind::text:
		return "text";
	}
	return "text";
}
} // namespace ear
