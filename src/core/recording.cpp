// A recording's claim value (ear/core/recording.hpp; docs/writing-a-node-type.md#recordings).
#include "ear/core/recording.hpp"

namespace ear {

std::string recordingClaimPath(std::string_view path) {
	std::string kept;
	while (!path.empty()) {
		const std::size_t slash = path.find('/');
		const std::string_view component = path.substr(0, slash);
		path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
		if (component.empty() || component == ".") continue;
		if (!kept.empty()) kept += '/';
		kept += component;
	}
	return kept;
}

} // namespace ear
