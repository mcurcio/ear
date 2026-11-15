// Every node type, as the all-type tests get them: from the generated list. The harness keeps a store and a recording
// in memory.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ear/all_node_types/all_node_types.hpp"

namespace ear::test {

inline std::vector<std::unique_ptr<const NodeType>> allTypes() { return nodes::allNodeTypes(); }

/// Settings `type` accepts, the one home of what the all-type tests need of a type: together they give every key it
/// declares. Throws, naming the type, when it has none here: Catch reports the test's failure.
inline const std::vector<NodeSettings>& validSettings(std::string_view type) {
	static const std::map<std::string, std::vector<NodeSettings>, std::less<>> valid{
		{"white", {NodeSettings::object()}},
		{"airplay", {{{"name", "Kitchen"}, {"airplay2", true}}}},
		{"file", {{{"path", "a.wav"}}}},
		{"card", {{{"card", "Pro24"}, {"playback", 2}, {"capture", 2}}}},
		{"rtp",
	     {{{"address", "0.0.0.0"},
	       {"port", 5004},
	       {"format", "L24"},
	       {"channels", 2},
	       {"rate", 48000},
	       {"latencyMs", 100}}}},
		// "port" only without "server", so it takes two objects to give every key.
		{"sendspin", {{{"port", 8928}, {"name", "Kitchen"}}, {{"server", "ws://a/b"}, {"name", "Kitchen"}}}}};
	const auto found = valid.find(type);
	if (found == valid.end()) {
		throw std::runtime_error("no valid settings for " + std::string(type) +
		                         ": add them to test/nodes/all_types.hpp");
	}
	return found->second;
}

} // namespace ear::test
