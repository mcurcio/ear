// Every node type, as the all-type tests get them: from the generated list, given real and empty folders beneath one
// temporary folder that lasts the test program (sendspin's config folder, and the recordings folder behind the harness's
// recordings, where the harness test writes "file"'s).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ear/all_node_types/all_node_types.hpp"
#include "ear/core/log.hpp"
#include "confined_recordings.hpp"
#include "ear/folder/confined_folder.hpp"
#include "temp_folder.hpp"

namespace ear::test {

/// `name`, made and opened beneath the temporary folder. Throws when it cannot be: Catch reports the test's failure.
inline std::shared_ptr<const folder::ConfinedFolder> openedForAllTypes(std::string_view name) {
	static const TempFolder root("all-types");
	auto opened = folder::ConfinedFolder::open(root.path() / name);
	if (!opened) throw std::runtime_error("cannot open " + std::string(name) + ": " + opened.error().detail);
	return std::make_shared<const folder::ConfinedFolder>(std::move(*opened));
}

inline const std::shared_ptr<const folder::ConfinedFolder>& recordingsFolder() {
	static const auto folder = openedForAllTypes("recordings");
	return folder;
}

inline const std::shared_ptr<const folder::ConfinedFolder>& configFolder() {
	static const auto folder = openedForAllTypes("config");
	return folder;
}

/// What the types' factories report at boot, kept for the test program's life (any thread).
struct BootLog final : LogSink {
	std::mutex mutex;
	std::vector<LogEntry> entries;
	void log(const LogEntry& entry) override {
		const std::scoped_lock lock(mutex);
		entries.push_back(entry);
	}
};

inline BootLog& bootLog() {
	static BootLog log;
	return log;
}

inline std::vector<std::unique_ptr<const NodeType>> allTypes() {
	return nodes::allNodeTypes({.config = configFolder(), .log = &bootLog()});
}

/// Recordings beneath the recordings folder, for the harness, as main.cpp's adapter makes them.
inline ConfinedRecordings& recordings() {
	static ConfinedRecordings adapter(recordingsFolder());
	return adapter;
}

/// Whether the all-type harness test runs `type` (docs/design/node-framework.md section 7.6): every type written against
/// a TypeSpec does. sendspin waits for slice 3 (its start still builds its worker, the key file and its threads, which
/// the harness does not fake). Throws, naming the type, when it is not here: Catch reports the test's failure.
inline bool runsInHarness(std::string_view type) {
	static const std::map<std::string, bool, std::less<>> runs{
		{"white", true}, {"file", true}, {"card", true}, {"rtp", true}, {"sendspin", false}};
	const auto found = runs.find(type);
	if (found == runs.end()) {
		throw std::runtime_error("no harness entry for " + std::string(type) +
		                         ": add it to runsInHarness in test/nodes/all_types.hpp");
	}
	return found->second;
}

/// Settings `type` accepts, the one home of what the all-type tests need of a type: together they give every key it
/// declares. Throws, naming the type, when it has none here: Catch reports the test's failure.
inline const std::vector<NodeSettings>& validSettings(std::string_view type) {
	static const std::map<std::string, std::vector<NodeSettings>, std::less<>> valid{
		{"white", {NodeSettings::object()}},
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
