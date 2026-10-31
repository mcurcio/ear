// The plain types of ear.conf's state on the wire (docs/design/knobly-config.md, "Wire additions";
// graph_file_state.hpp): the one home of the severity, format key, cause and file refusal names.
#include "ear/control/graph_file_state.hpp"

#include <format>
#include <type_traits>

namespace ear::control {

std::string_view severityName(Severity severity) noexcept {
	switch (severity) {
	case Severity::error: return "error";
	case Severity::warning: return "warning";
	}
	return "error";
}

std::string_view formatKeyName(FormatKey key) noexcept {
	switch (key) {
	case FormatKey::graphFrequency: return "graphFrequency";
	case FormatKey::blockSize: return "blockSize";
	}
	return "graphFrequency";
}

std::string_view causeName(FileChangeCause cause) noexcept {
	switch (cause) {
	case FileChangeCause::edit: return "edit";
	case FileChangeCause::disk: return "disk";
	case FileChangeCause::write: return "write";
	}
	return "edit";
}

std::string_view fileRefusalCode(const FileRefusal& refusal) noexcept {
	return std::visit(
		[](const auto& r) -> std::string_view {
			using R = std::decay_t<decltype(r)>;
			if constexpr (std::is_same_v<R, FileChanged>) return "file-changed";
			else if constexpr (std::is_same_v<R, InvalidText>) return "invalid-text";
			else if constexpr (std::is_same_v<R, FileInvalid>) return "file-invalid";
			else {
				static_assert(std::is_same_v<R, SharedDefinition>);
				return "shared-definition";
			}
		},
		refusal);
}

std::string describe(const FileRefusal& refusal) {
	if (const auto* shared = std::get_if<SharedDefinition>(&refusal))
		return std::format("{} {}:{}", fileRefusalCode(refusal), shared->place.line, shared->place.column);
	return std::string(fileRefusalCode(refusal));
}

} // namespace ear::control
