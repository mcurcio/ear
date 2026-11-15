// Programs' words and paths (ear/core/program.hpp; docs/writing-a-node-type.md#programs): a program's arguments as a
// type writes them, the shared-memory names it may need, the one wording of a program's end and of its readiness
// deadline, and the one spelling of a configuration's runtime file paths.
#include "ear/core/program.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>

namespace ear {

ProgramArg::ProgramArg(const char* text) : parts{part::Text{text}} {}
ProgramArg::ProgramArg(std::string text) : parts{part::Text{std::move(text)}} {}
ProgramArg::ProgramArg(ProgramPart piece) : parts{std::move(piece)} {}
ProgramArg::ProgramArg(std::initializer_list<ProgramPart> given) : parts(given) {}

std::optional<SharedMemoryName> SharedMemoryName::parse(std::string_view text) {
	if (text.size() < 2 || text.size() > 63 || text.front() != '/') return std::nullopt;
	const std::string_view rest = text.substr(1);
	if (rest == "." || rest == "..") return std::nullopt;
	const bool plain = std::ranges::all_of(rest, [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
		       c == '-';
	});
	if (!plain) return std::nullopt;
	return SharedMemoryName(std::string(text));
}

std::string programExitDetail(std::string_view name, const ProgramExit& exit) {
	std::string detail(name);
	// Ear ended it: how it then ended is only its answer to ear's own SIGTERM, so the cause is the whole story.
	if (!exit.removed.empty())
		return detail + " was ended: shared memory " + exit.removed + ", which it needs, was removed";
	if (exit.how == ProgramExit::How::exited) {
		detail += " exited with status " + std::to_string(exit.number);
	} else {
		const char* abbrev = ::sigabbrev_np(exit.number);
		detail += " was killed by signal " + std::to_string(exit.number) + " (SIG" + (abbrev ? abbrev : "?") + ")";
	}
	if (!exit.lastLine.empty()) detail += ": " + exit.lastLine;
	return detail;
}

std::string programOverdueDetail(std::string_view name, std::span<const SharedMemoryName> needs) {
	const std::string within =
		" within " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(programReadinessBound).count()) +
		" s";
	std::string detail(name);
	if (needs.empty()) return detail + " did not start" + within;
	detail += " did not start, or find ";
	for (std::size_t i = 0; i < needs.size(); ++i) {
		if (i > 0) detail += ", ";
		detail += needs[i].text();
	}
	return detail + "," + within;
}

std::string runtimePath(std::string_view folder, const NodeName& node, std::uint64_t configuration,
                        std::string_view what) {
	std::string path(folder);
	path += '/';
	path += node;
	path += '.';
	path += std::to_string(configuration);
	path += '.';
	path += what;
	return path;
}

} // namespace ear
