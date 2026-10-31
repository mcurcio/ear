// What the graph file keeper's tests share: a temporary graph file folder, the record's example
// file, a hand edit made the way editors save (a temporary file renamed over ear.conf), and
// listeners that write what they are told into one ordered log.
#pragma once
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "ear/control/requests.hpp"
#include "ear/core/desired_graph.hpp"

namespace ear::config::test {

/// The record's file format example (docs/design/knobly-config.md, "File format").
inline const std::string recordExample = R"(graph_frequency 48000;
block_size 256;
@set $ch 2;                      # comments survive every edit
@snippet card_out $id {          # written once, used by both cards
    card $id;
    playback $ch;
}
node white noise;
node file rec { path "take1.wav"; }
node card desk { @use card_out MOTU; }
node card porch { @use card_out USB2; }
route noise:MONO rec:FL;
route noise:MONO desk:playback_1;
)";

/// `text` with its first `from` replaced by `to` (which must be there).
inline std::string replaced(std::string text, const std::string& from, const std::string& to) {
	const auto at = text.find(from);
	if (at == std::string::npos) throw std::logic_error("replaced: '" + from + "' is not in the text");
	return text.replace(at, from.size(), to);
}

class TempDir {
public:
	explicit TempDir(const std::string& name)
	    : path_(std::filesystem::temp_directory_path() / ("ear_config_" + name + "_" + std::to_string(getpid()))) {
		std::filesystem::remove_all(path_);
		std::filesystem::create_directories(path_);
	}
	~TempDir() {
		std::error_code ignored;
		std::filesystem::remove_all(path_, ignored);
	}
	TempDir(const TempDir&) = delete;
	TempDir& operator=(const TempDir&) = delete;
	const std::filesystem::path& path() const noexcept { return path_; }

private:
	std::filesystem::path path_;
};

inline std::string contents(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// A hand edit as editors save: write a temporary file beside it, then rename it over `path`.
inline void handEdit(const std::filesystem::path& path, const std::string& text) {
	const auto temporary = path.parent_path() / ".hand-edit.tmp";
	std::ofstream(temporary, std::ios::binary) << text;
	std::filesystem::rename(temporary, path);
}

/// The names in a folder.
inline std::vector<std::string> names(const std::filesystem::path& folder) {
	std::vector<std::string> all;
	for (const auto& entry : std::filesystem::directory_iterator(folder)) all.push_back(entry.path().filename().string());
	return all;
}

/// What the listeners and replies were told, in order, with the main-loop turn each line came in.
struct Log {
	std::vector<std::string> lines;
	std::vector<int> turns;
	int turn = 0;   // the main loop's current turn, kept by the test's main-loop executor
	void push(std::string line) {
		lines.push_back(std::move(line));
		turns.push_back(turn);
	}
	void clear() {
		lines.clear();
		turns.clear();
	}
	int turnOf(const std::string& line) const {
		for (std::size_t i = 0; i < lines.size(); ++i)
			if (lines[i] == line) return turns[i];
		return -1;
	}
};

/// Graph events, as one log line each ("node.added desk", "route.removed noise:MONO desk:playback_1").
class GraphLog final : public DesiredNodeListener, public DesiredRouteListener {
public:
	explicit GraphLog(Log& log) : log_(log) {}
	void nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig>) override { log_.push("node.added " + node.name); }
	void nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig>) override { log_.push("node.updated " + node.name); }
	void nodeRemoved(const NodeName& node) override { log_.push("node.removed " + node); }
	void routeAdded(const Route& route) override { log_.push("route.added " + text(route)); }
	void routeRemoved(const Route& route) override { log_.push("route.removed " + text(route)); }

private:
	static std::string text(const Route& r) { return r.from.node + ":" + r.from.port + " " + r.to.node + ":" + r.to.port; }
	Log& log_;
};

/// Graph file changes as log lines ("file edit", "file disk dropped=1", "written <v>"), each kept whole.
class FileLog final : public control::GraphFileListener {
public:
	explicit FileLog(Log& log) : log_(log) {}
	std::vector<control::GraphFileChanged> changes;
	std::vector<std::string> written;
	void graphFileChanged(const control::GraphFileChanged& change) override {
		changes.push_back(change);
		std::string line = "file " + std::string(control::causeName(change.cause));
		if (change.droppedEdits > 0) line += " dropped=" + std::to_string(change.droppedEdits);
		log_.push(std::move(line));
	}
	void graphFileWritten(const std::string& version) override {
		written.push_back(version);
		log_.push("written");
	}

private:
	Log& log_;
};

} // namespace ear::config::test
