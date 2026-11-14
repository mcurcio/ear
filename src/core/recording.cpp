// A recording's path rule and claim value (ear/core/recording.hpp; docs/writing-a-node-type.md#recordings).
#include "ear/core/recording.hpp"

namespace ear {
namespace {

/// Calls `each` with every component of `path` split at '/', empty ones included.
template <class Each> void components(std::string_view path, Each each) {
	while (!path.empty()) {
		const std::size_t slash = path.find('/');
		each(path.substr(0, slash));
		path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
	}
}

} // namespace

std::optional<std::string> recordingPathRefused(std::string_view path) {
	bool climbs = false;
	components(path, [&](std::string_view component) { climbs = climbs || component == ".."; });
	if (path.starts_with('/') || climbs)
		return std::string(R"(must be relative to the recordings folder, without "..")");
	return std::nullopt;
}

std::string recordingClaimPath(std::string_view path) {
	std::string kept;
	components(path, [&](std::string_view component) {
		if (component.empty() || component == ".") return;
		if (!kept.empty()) kept += '/';
		kept += component;
	});
	return kept;
}

} // namespace ear
