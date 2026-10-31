// What the system tests share (docs/design/knobly-config.md, S5 criterion T9): a fresh directory
// per test holding the graph file folder (config/, with ear.conf), the recordings folder
// (recordings/) and anything outside both; the real out/ear started on it with its control port on
// 127.0.0.1 at a free port; a JSON-RPC client on one WebSocket; and the WAV checks. Every wait is
// for an event (a line ear prints, a message, ear's exit); a deadline only fails a test.
#pragma once

#include <sndfile.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "catch2/catch.hpp"
#include "child_process.hpp"
#include "ws_client.hpp"

namespace ear::test {

using namespace std::chrono_literals;
using nlohmann::json;

/// Fails a test whose awaited event never comes; never paces one.
inline constexpr auto deadline = 10s;
inline constexpr std::string_view listeningPrefix = "control listening on 127.0.0.1:";

/// A fresh directory for one test's folders and files, removed afterwards.
class TempDirectory {
public:
	TempDirectory() {
		std::string pattern = (std::filesystem::temp_directory_path() / "ear-system-XXXXXX").string();
		if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
		path_ = pattern;
	}
	~TempDirectory() { std::filesystem::remove_all(path_); }
	TempDirectory(const TempDirectory&) = delete;
	TempDirectory& operator=(const TempDirectory&) = delete;
	const std::filesystem::path& path() const noexcept { return path_; }
	std::filesystem::path config() const { return path_ / "config"; }          // --config-dir
	std::filesystem::path recordings() const { return path_ / "recordings"; }  // --recordings-dir
	std::filesystem::path graphFile() const { return config() / "ear.conf"; }

private:
	std::filesystem::path path_;
};

inline std::string readText(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	std::ostringstream text;
	text << file.rdbuf();
	return text.str();
}

/// Writes ear.conf (the config folder made if missing) as a hand editor that saves by rename
/// does: a whole new file swapped in, so the folder watch sees one change.
inline void writeGraphFile(const TempDirectory& directory, const std::string& text) {
	std::filesystem::create_directories(directory.config());
	const auto staging = directory.path() / "ear.conf.saving";   // outside the folder ear watches
	std::ofstream(staging, std::ios::binary) << text;
	std::filesystem::rename(staging, directory.graphFile());
}

/// `graph_frequency <rate>; block_size <block>;`, then `body`.
inline std::string graphFileText(const std::string& body, std::uint32_t rate = 48000, std::uint32_t block = 256) {
	return "graph_frequency " + std::to_string(rate) + ";\nblock_size " + std::to_string(block) + ";\n" + body;
}

/// white "noise" routed to file "rec":FL (rec.wav in the recordings folder), plus `more`.
inline std::string noiseToRec(const std::string& to = "FL", const std::string& more = "") {
	return graphFileText("node white noise;\nnode file rec { path \"rec.wav\"; }\nroute noise:MONO rec:" + to + ";\n" + more);
}

/// --config-dir and --recordings-dir in `directory`, the control port on 127.0.0.1 at a free port,
/// then `extra`.
inline std::vector<std::string> earArguments(const TempDirectory& directory, const std::vector<std::string>& extra = {}) {
	std::vector<std::string> arguments{"--config-dir", directory.config().string(), "--recordings-dir",
	                                   directory.recordings().string(), "--control-address", "127.0.0.1",
	                                   "--control-port", "0"};
	arguments.insert(arguments.end(), extra.begin(), extra.end());
	return arguments;
}

inline std::string linkedLine(const std::string& from, const std::string& to) { return "route " + from + " -> " + to + " linked"; }

/// The real out/ear on `directory`, ready once it printed `readyLine` (or, with readyPrefix, a line
/// starting with it); its control port is the one its "control listening" line names.
class Ear {
public:
	struct Ready { std::string line; bool prefix = false; };

	Ear(const TempDirectory& directory, const Ready& ready, const std::vector<std::string>& extra = {})
	    : process_(EAR_BINARY, earArguments(directory, extra)) {
		const bool isReady = ready.prefix ? process_.waitForLineStartingWith(ready.line, deadline).has_value()
		                                  : process_.waitForLine(ready.line, deadline);
		INFO("ear printed:\n" << process_.output());
		REQUIRE(isReady);
		const auto listening = process_.waitForLineStartingWith(listeningPrefix, deadline);
		REQUIRE(listening);
		port_ = static_cast<std::uint16_t>(std::stoul(listening->substr(listeningPrefix.size())));
	}

	ChildProcess& process() noexcept { return process_; }
	std::uint16_t port() const noexcept { return port_; }

	/// SIGTERM, then ear's exit status.
	std::optional<int> stop() {
		process_.signal(SIGTERM);
		return process_.waitForExit(deadline);
	}

	/// SIGTERM with `client` connected: ear finishes the close handshake with each client before
	/// it exits, so the client reads until ear closes it (with 1001), then ear's exit status.
	template <typename Client>
	std::optional<int> stop(Client& client) {
		process_.signal(SIGTERM);
		CHECK(client.readUntilClosed() == 1001);
		return process_.waitForExit(deadline);
	}

private:
	ChildProcess process_;
	std::uint16_t port_ = 0;
};

/// A JSON-RPC client on one WebSocket: calls wait for their own response and keep every
/// notification that arrived meanwhile, in order.
class Rpc {
public:
	explicit Rpc(std::uint16_t port) : ws_(port) {}

	/// Sends one request and returns its response.
	json call(const std::string& method, const json& params = nullptr) {
		const int id = nextId_++;
		json request = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
		if (!params.is_null()) request["params"] = params;
		ws_.send(request.dump());
		while (true) {
			json message = json::parse(ws_.read());
			if (message.contains("id") && message["id"] == id) return message;
			notifications.push_back(std::move(message));
		}
	}

	/// Reads until a notification after index `from` matching `wanted` arrives (or one already did)
	/// and returns it.
	json waitFor(const std::function<bool(const json&)>& wanted, std::size_t from = 0) {
		for (std::size_t i = from; i < notifications.size(); ++i) {
			if (wanted(notifications[i])) return notifications[i];
		}
		try {
			while (true) {
				json message = json::parse(ws_.read());
				notifications.push_back(message);
				if (wanted(message)) return message;
			}
		} catch (const TimedOut&) {
			FAIL("the awaited notification never came; notifications: " << json(notifications).dump());
			throw;
		}
	}

	/// Reads until the server closes the WebSocket (answering its close, as a browser does) and
	/// returns the close code.
	std::uint16_t readUntilClosed() {
		try {
			while (true) notifications.push_back(json::parse(ws_.read()));
		} catch (const Closed& closed) {
			return closed.code;
		}
	}

	/// Index in `notifications` of the first one at or after `from` with `method` and, in params,
	/// every key of `has`; notifications.size() if none.
	std::size_t indexOf(const std::string& method, const json& has, std::size_t from = 0) const {
		for (std::size_t i = from; i < notifications.size(); ++i) {
			if (matches(notifications[i], method, has)) return i;
		}
		return notifications.size();
	}

	static bool matches(const json& message, const std::string& method, const json& has) {
		if (message.value("method", "") != method) return false;
		const json& params = message["params"];
		for (const auto& [key, value] : has.items()) {
			if (!params.contains(key) || params[key] != value) return false;
		}
		return true;
	}

	std::vector<json> notifications;

private:
	WsClient ws_;
	int nextId_ = 1;
};

/// The RIFF header's size field matches the file: the writer closed the file properly.
inline bool headerFinished(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	if (bytes.size() < 8) return false;
	const std::uint32_t riffSize =
	    bytes[4] | (bytes[5] << 8U) | (bytes[6] << 16U) | (static_cast<std::uint32_t>(bytes[7]) << 24U);
	return riffSize == bytes.size() - 8;
}

struct Wav {
	SF_INFO info{};
	std::vector<float> samples;   // interleaved
};

inline Wav readWav(const std::filesystem::path& path) {
	Wav wav;
	SNDFILE* file = sf_open(path.c_str(), SFM_READ, &wav.info);
	REQUIRE(file != nullptr);
	wav.samples.resize(static_cast<std::size_t>(wav.info.frames * wav.info.channels));
	CHECK(sf_readf_float(file, wav.samples.data(), wav.info.frames) == wav.info.frames);
	sf_close(file);
	return wav;
}

/// A finished 32-bit float stereo WAV at `rate` with uniform white noise on channel `noisy` for at
/// least half a second, unbroken, and exact zeros on the other.
inline void checkNoiseOnOneChannel(const std::filesystem::path& path, int noisy, std::uint32_t rate) {
	CHECK(headerFinished(path));
	const Wav wav = readWav(path);
	CHECK(wav.info.samplerate == static_cast<int>(rate));
	CHECK(wav.info.channels == 2);
	CHECK((wav.info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAV);
	CHECK((wav.info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_FLOAT);

	const auto frames = static_cast<std::size_t>(wav.info.frames);
	const int silent = 1 - noisy;
	std::size_t first = frames;
	std::size_t last = 0;
	std::size_t nonzeroOnSilent = 0;
	for (std::size_t frame = 0; frame < frames; ++frame) {
		if (wav.samples[frame * 2 + silent] != 0.0F) ++nonzeroOnSilent;
		if (wav.samples[frame * 2 + noisy] != 0.0F) {
			if (first == frames) first = frame;
			last = frame;
		}
	}
	CHECK(nonzeroOnSilent == 0);
	REQUIRE(first < frames);
	const std::size_t span = last - first + 1;
	CHECK(span >= rate / 2);

	// Uniform noise in [-1, 1) has RMS 1/sqrt(3) = 0.577. A gap (a lost block) would show as
	// a run of zeros; a single exact zero is a legitimate noise sample (one in 2^24).
	double sumOfSquares = 0.0;
	std::size_t zeroRuns = 0;
	for (std::size_t frame = first; frame <= last; ++frame) {
		const float sample = wav.samples[frame * 2 + noisy];
		sumOfSquares += static_cast<double>(sample) * sample;
		if (sample == 0.0F && wav.samples[(frame - 1) * 2 + noisy] == 0.0F) ++zeroRuns;
	}
	CHECK(zeroRuns == 0);
	const double rms = std::sqrt(sumOfSquares / static_cast<double>(span));
	CHECK(rms == Approx(0.577).margin(0.01));
}

} // namespace ear::test
