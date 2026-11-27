// Every node type, as the all-type tests get them: from the generated list. The harness keeps a store and a recording
// in memory.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ear/all_node_types/all_node_types.hpp"
#include "ear/core/settings_table.hpp"

namespace ear::test {

inline std::vector<std::unique_ptr<const NodeType>> allTypes() { return nodes::allNodeTypes(); }

/// Settings `type` accepts, the one home of what the all-type tests need of a type: together they give every key it
/// declares. Throws, naming the type, when it has none here: Catch reports the test's failure.
inline const std::vector<NodeSettings>& validSettings(std::string_view type) {
	static const std::map<std::string, std::vector<NodeSettings>, std::less<>> valid{
		{"white", {NodeSettings::object()}},
		// A family's key is given by a member (docs/writing-a-node-type.md#indexed-families).
		{"gain", {{{"channels", 2}, {"gainDb", -6.0}, {"mute", false}, {"trimDb.1", 0.5}, {"invert.2", true}}}},
		{"airplay", {{{"name", "Kitchen"}, {"airplay2", true}}}},
		{"file", {{{"path", "a.wav"}}}},
		{"card", {{{"card", "Pro24"}, {"playback", 2}, {"capture", 2}}}},
		// Two, differing in every key a change makes in place, so the all-type change in place has a pair.
		{"rtp",
	     {{{"address", "0.0.0.0"},
	       {"port", 5004},
	       {"format", "L24"},
	       {"channels", 2},
	       {"rate", 48000},
	       {"latencyMs", 100}},
	      {{"address", "239.69.0.1"},
	       {"port", 5006},
	       {"format", "L16"},
	       {"channels", 2},
	       {"rate", 44100},
	       {"latencyMs", 200}}}},
		// "port" only without "server", so it takes two objects to give every key.
		{"sendspin", {{{"port", 8928}, {"name", "Kitchen"}}, {{"server", "ws://a/b"}, {"name", "Kitchen"}}}}};
	const auto found = valid.find(type);
	if (found == valid.end()) {
		throw std::runtime_error("no valid settings for " + std::string(type) +
		                         ": add them to test/nodes/all_types.hpp");
	}
	return found->second;
}

/// The key `settings` give row `row` of `type` by: a plain row's own key; for an indexed family, the first of its members
/// they hold, else its first member, `<key>.1` (docs/writing-a-node-type.md#indexed-families).
inline std::string givenKey(const NodeType& type, std::string_view row, const NodeSettings& settings) {
	for (const SettingRow* declared : type.schema().settings) {
		if (declared->key != row || declared->maxMembers == 0) continue;
		for (std::uint32_t member = 1; member <= declared->maxMembers; ++member) {
			std::string key = std::string(row) + "." + std::to_string(member);
			if (settings.contains(key)) return key;
		}
		return std::string(row) + ".1";
	}
	return std::string(row);
}

} // namespace ear::test
