// Programs' words and paths (ear/core/program.hpp; docs/writing-a-node-type.md#programs): a program's arguments as a
// type writes them, the one wording of a program's end, and the one spelling of a configuration's runtime file paths.
#include "ear/core/program.hpp"

#include <cstring>
#include <string>
#include <utility>

namespace ear {

ProgramArg::ProgramArg(const char* text) : parts{part::Text{text}} {}
ProgramArg::ProgramArg(std::string text) : parts{part::Text{std::move(text)}} {}
ProgramArg::ProgramArg(ProgramPart piece) : parts{std::move(piece)} {}
ProgramArg::ProgramArg(std::initializer_list<ProgramPart> given) : parts(given) {}

std::string programExitDetail(std::string_view name, const ProgramExit& exit) {
	std::string detail(name);
	if (exit.how == ProgramExit::How::exited) {
		detail += " exited with status " + std::to_string(exit.number);
	} else {
		const char* abbrev = ::sigabbrev_np(exit.number);
		detail += " was killed by signal " + std::to_string(exit.number) + " (SIG" + (abbrev ? abbrev : "?") + ")";
	}
	if (!exit.lastLine.empty()) detail += ": " + exit.lastLine;
	return detail;
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
