// What the system tests share (docs/graph-file.md): a fresh directory
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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "catch2/catch.hpp"
#include "child_process.hpp"
#include "statistics.hpp"
#include "temp_folder.hpp"
#include "ws_client.hpp"

namespace ear::test {

using namespace std::chrono_literals;
using nlohmann::json;

/// Fails a test whose awaited event never comes; never paces one.
inline constexpr auto deadline = 10s;
/// ear's last record at a clean stop (docs/output-records.md#exit).
inline constexpr std::string_view stoppedLine = "ear: stopped";
/// Fails a test whose ear never ends after its last record. What is left then is not ear's work but its destructors
/// and the kernel's teardown of the process, which a starved host can stretch to seconds (closing an inotify
/// descriptor waits for the host's RCU grace period: docs/architecture.md#stop-sigint-or-sigterm), so this catches a
/// hang and judges no time.
inline constexpr auto exitDeadline = 60s;
inline constexpr std::string_view listeningPrefix = "control listening on 127.0.0.1:";

/// A fresh folder for one test, holding its config folder and recordings folder, removed afterwards.
class TempDirectory : public TempFolder {
public:
	TempDirectory() : TempFolder("system") {}
	std::filesystem::path config() const { return path() / "config"; }         // --config-dir
	std::filesystem::path recordings() const { return path() / "recordings"; } // --recordings-dir
	std::filesystem::path graphFile() const { return config() / "ear.conf"; }
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
	const auto staging = directory.path() / "ear.conf.saving"; // outside the folder ear watches
	std::ofstream(staging, std::ios::binary) << text;
	std::filesystem::rename(staging, directory.graphFile());
}

/// `graph_frequency <rate>; block_size <block>;`, then `body`.
inline std::string graphFileText(const std::string& body, std::uint32_t rate = 48000, std::uint32_t block = 256) {
	return "graph_frequency " + std::to_string(rate) + ";\nblock_size " + std::to_string(block) + ";\n" + body;
}

/// white "noise" routed to file "rec":record.left (rec.wav in the recordings folder), plus `more`.
inline std::string noiseToRec(const std::string& to = "record.left", const std::string& more = "") {
	return graphFileText("node white noise;\nnode file rec { path \"rec.wav\"; }\nroute noise:noise.mono rec:" + to +
	                     ";\n" + more);
}

/// --config-dir and --recordings-dir in `directory`, the control port on 127.0.0.1 at a free port,
/// then `extra`.
inline std::vector<std::string> earArguments(const TempDirectory& directory,
                                             const std::vector<std::string>& extra = {}) {
	std::vector<std::string> arguments{"--config-dir",      directory.config().string(),
	                                   "--recordings-dir",  directory.recordings().string(),
	                                   "--control-address", "127.0.0.1",
	                                   "--control-port",    "0"};
	arguments.insert(arguments.end(), extra.begin(), extra.end());
	return arguments;
}

/// After SIGTERM: ear's stop, judged by its own last record (`ear: stopped`, under the shared deadline), then its exit
/// status once the process has ended. Nothing if the record never came, the process did not end, or a signal ended it.
inline std::optional<int> waitForStop(ChildProcess& ear) {
	if (!ear.waitForLine(stoppedLine, deadline)) return std::nullopt;
	return ear.waitForExit(exitDeadline);
}

/// The `t` of the first record whose msg is `msg`, on ear's clock; nothing if there is none.
inline std::optional<std::chrono::sys_time<std::chrono::microseconds>> recordTime(const ChildProcess& ear,
                                                                                  std::string_view msg) {
	for (const json& record : ear.records()) {
		if (record.value("msg", "") != msg) continue;
		std::istringstream text(record.value("t", ""));
		std::chrono::sys_time<std::chrono::microseconds> t;
		text >> std::chrono::parse("%FT%TZ", t);
		if (!text) return std::nullopt;
		return t;
	}
	return std::nullopt;
}

/// How long ear's stop took to tear `node` down, from its records: `stopping: removing` to `removed`, on ear's clock.
/// Nothing if either record is missing.
inline std::optional<std::chrono::microseconds> removalTook(const ChildProcess& ear, const std::string& node) {
	const auto stopping = recordTime(ear, "node " + node + " stopping: removing");
	std::optional<std::chrono::sys_time<std::chrono::microseconds>> removed;
	for (const json& record : ear.records()) {
		const std::string msg = record.value("msg", "");
		if (msg.starts_with("node " + node + " removed ")) {
			removed = recordTime(ear, msg);
			break;
		}
	}
	if (!stopping || !removed) return std::nullopt;
	return *removed - *stopping;
}

inline std::string linkedLine(const std::string& from, const std::string& to) {
	return "route " + from + " -> " + to + " linked";
}

/// The real out/ear on `directory`, ready once it printed `readyLine` (or, with readyPrefix, a line
/// starting with it); its control port is the one its "control listening" line names.
class Ear {
public:
	struct Ready {
		std::string line;
		bool prefix = false;
	};

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

	/// SIGTERM, then ear's exit status (waitForStop).
	std::optional<int> stop() {
		process_.signal(SIGTERM);
		return waitForStop(process_);
	}

	/// SIGTERM with `client` connected: ear finishes the close handshake with each client before
	/// it exits, so the client reads until ear closes it (with 1001), then ear's exit status.
	template <typename Client> std::optional<int> stop(Client& client) {
		process_.signal(SIGTERM);
		CHECK(client.readUntilClosed() == 1001);
		return waitForStop(process_);
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
	std::vector<float> samples; // interleaved
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

/// A 32-bit float WAV a file node is still writing, followed by what is appended to it: its RIFF chunks are read once
/// for the data chunk's start (the header's sizes are filled in only when the writer finishes), then each look reads only
/// the bytes past where the last one stopped, whole frames only, so following it costs ear nothing. An appended byte is
/// written before the file grows over it, so a frame read is a frame written. The finished file is what a test judges.
class WavFollower {
public:
	explicit WavFollower(std::filesystem::path path) : path_(std::move(path)) {}

	/// The whole frames appended since the last look, interleaved (empty before the header is written).
	std::span<const float> next() {
		fresh_.clear();
		std::ifstream file(path_, std::ios::binary);
		if (!file) return {};
		if (offset_ == 0 && !findData(file)) return {};
		file.clear();
		file.seekg(static_cast<std::streamoff>(offset_));
		const std::size_t frameBytes = static_cast<std::size_t>(channels_) * sizeof(float);
		std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
		const std::size_t frames = bytes.size() / frameBytes;
		fresh_.resize(frames * static_cast<std::size_t>(channels_));
		std::memcpy(fresh_.data(), bytes.data(), frames * frameBytes);
		offset_ += frames * frameBytes;
		frames_ += frames;
		return fresh_;
	}
	int channels() const noexcept { return channels_; }
	/// Frames read so far.
	std::size_t frames() const noexcept { return frames_; }

private:
	bool findData(std::ifstream& file) {
		std::vector<unsigned char> bytes(4096);
		file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		bytes.resize(static_cast<std::size_t>(file.gcount()));
		const auto word = [&](std::size_t at, std::size_t size) {
			std::uint32_t value = 0;
			for (std::size_t i = 0; i < size; ++i) value |= static_cast<std::uint32_t>(bytes[at + i]) << (8U * i);
			return value;
		};
		if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
		    std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
			return false;
		for (std::size_t at = 12; at + 8 <= bytes.size(); at += 8 + word(at + 4, 4) + (word(at + 4, 4) & 1U)) {
			if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0 && at + 16 <= bytes.size()) {
				channels_ = static_cast<int>(word(at + 10, 2));
			} else if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
				if (channels_ <= 0) return false;
				offset_ = at + 8;
				return true;
			}
		}
		return false;
	}

	std::filesystem::path path_;
	std::size_t offset_ = 0; // past the last whole frame read; 0 until the data chunk is found
	int channels_ = 0;
	std::size_t frames_ = 0;
	std::vector<float> fresh_;
};

/// Until what a file node appends to `path` satisfies `enough`, told only the new frames each look (`enough` keeps its
/// own count): the WAV holding what a check needs is the event, so a loaded host only makes the wait longer;
/// `patience` only fails the test.
inline bool waitForWav(const std::filesystem::path& path,
                       const std::function<bool(std::span<const float> fresh, std::size_t channels)>& enough,
                       std::chrono::milliseconds patience) {
	WavFollower wav(path);
	const auto until = std::chrono::steady_clock::now() + patience;
	for (;;) {
		const std::span<const float> fresh = wav.next();
		if (wav.channels() > 0 && enough(fresh, static_cast<std::size_t>(wav.channels()))) return true;
		if (std::chrono::steady_clock::now() >= until) return false;
		std::this_thread::sleep_for(50ms); // between looks at the WAV, which decide
	}
}

/// Until the WAV a file node writes at `path` holds a second of noise on channel `noisy`, at `rate`: the recording
/// window checkNoiseOnOneChannel needs, waited for as an event.
inline bool waitForNoiseSecond(const std::filesystem::path& path, int noisy, std::uint32_t rate) {
	std::optional<std::size_t> sound; // frames of it so far, once heard
	return waitForWav(
		path,
		[&](std::span<const float> fresh, std::size_t channels) {
			for (std::size_t at = static_cast<std::size_t>(noisy); at < fresh.size(); at += channels) {
				if (sound) ++*sound;
				else if (fresh[at] != 0.0F) sound = 1;
			}
			return sound.value_or(0) >= rate;
		},
		4 * deadline);
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
	// The noise's RMS within 5 standard deviations for the frames measured (docs/testing.md#statistics-from-the-count),
	// past the drive's 5 ms fade-in (docs/engine.md#drive-and-fade), which would weigh on it.
	const std::size_t fade = rate * 5 / 1000;
	double fadedSquares = 0.0;
	for (std::size_t frame = first; frame < first + fade && frame <= last; ++frame) {
		const float sample = wav.samples[frame * 2 + noisy];
		fadedSquares += static_cast<double>(sample) * sample;
	}
	const std::size_t measured = span > fade ? span - fade : 0;
	const double rms = measured == 0 ? 0.0 : std::sqrt((sumOfSquares - fadedSquares) / static_cast<double>(measured));
	INFO("noise RMS " << rms << " over " << measured << " frames, sigma " << noiseRmsSigma(measured));
	CHECK(noiseRmsWithin(rms, measured, 5.0));
}

} // namespace ear::test
