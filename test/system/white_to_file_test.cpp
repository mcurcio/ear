// The owner's end-to-end test (docs/design/m1-white-to-file.md section 3, S6 criteria 1-4): the
// real out/ear runs a graph file that routes white noise into one channel of a file node, is
// stopped with SIGTERM and leaves a finished WAV file with the noise on that channel only.
#include <sndfile.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "catch2/catch.hpp"
#include "child_process.hpp"

namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t graphFrequency = 48000;
constexpr std::uint32_t blockSize = 256;
/// Fails a test whose awaited event never comes; never paces one.
constexpr auto deadline = 10s;

/// A fresh directory for one test's graph file and WAV file, removed afterwards.
class TempDirectory {
public:
	TempDirectory() {
		std::string pattern = (std::filesystem::temp_directory_path() / "ear-system-XXXXXX").string();
		if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
		path_ = pattern;
	}
	~TempDirectory() { std::filesystem::remove_all(path_); }
	const std::filesystem::path& path() const noexcept { return path_; }

private:
	std::filesystem::path path_;
};

/// A graph file routing white noise "noise" to port `to` of file node "rec", which writes
/// rec.wav in the recordings folder.
std::filesystem::path writeGraph(const TempDirectory& directory, const std::string& to) {
	const nlohmann::json graph = {
	    {"graphFrequency", graphFrequency},
	    {"blockSize", blockSize},
	    {"nodes",
	     {{{"name", "noise"}, {"type", "white"}},
	      {{"name", "rec"}, {"type", "file"}, {"settings", {{"path", "rec.wav"}}}}}},
	    {"routes", {{{"from", {{"node", "noise"}, {"port", "MONO"}}}, {"to", {{"node", "rec"}, {"port", to}}}}}},
	};
	const auto path = directory.path() / "ear.json";
	std::ofstream(path) << graph.dump(2);
	return path;
}

struct Wav {
	SF_INFO info{};
	std::vector<float> samples;   // interleaved
};

Wav readWav(const std::filesystem::path& path) {
	Wav wav;
	SNDFILE* file = sf_open(path.c_str(), SFM_READ, &wav.info);
	REQUIRE(file != nullptr);
	wav.samples.resize(static_cast<std::size_t>(wav.info.frames * wav.info.channels));
	CHECK(sf_readf_float(file, wav.samples.data(), wav.info.frames) == wav.info.frames);
	sf_close(file);
	return wav;
}

/// The RIFF header's size field matches the file: the writer closed the file properly.
bool headerFinished(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	if (bytes.size() < 8) return false;
	const std::uint32_t riffSize = bytes[4] | (bytes[5] << 8U) | (bytes[6] << 16U) | (static_cast<std::uint32_t>(bytes[7]) << 24U);
	return riffSize == bytes.size() - 8;
}

/// Runs ear on a graph routing noise to rec:`to` for about a second after the route is linked,
/// stops it with SIGTERM and checks the WAV: noise on channel `noisy`, exact zeros on the other.
void checkNoiseOnOneChannel(const std::string& to, int noisy) {
	TempDirectory directory;
	const auto graph = writeGraph(directory, to);
	const auto recordings = directory.path() / "recordings";
	ear::test::ChildProcess ear(EAR_BINARY, {"--config", graph.string(), "--control-port", "0", "--recordings-dir",
	                                         recordings.string()});

	const bool linked = ear.waitForLine("route noise:MONO -> rec:" + to + " linked", deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(linked);
	// The recording window the criterion asks for; the checks below need at least half of it.
	std::this_thread::sleep_for(1s);
	ear.signal(SIGTERM);
	const auto exitStatus = ear.waitForExit(deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(exitStatus == 0);
	CHECK(ear.hasLine("node rec stopped dropped=0"));

	const auto wavPath = recordings / "rec.wav";
	CHECK(headerFinished(wavPath));
	const Wav wav = readWav(wavPath);
	CHECK(wav.info.samplerate == static_cast<int>(graphFrequency));
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
	CHECK(span >= graphFrequency / 2);

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

} // namespace

TEST_CASE("White noise routed to file:FL lands on FL only", "[system]") {
	checkNoiseOnOneChannel("FL", 0);
}

TEST_CASE("White noise routed to file:FR lands on FR only", "[system]") {
	checkNoiseOnOneChannel("FR", 1);
}

TEST_CASE("A route to an undeclared port stops ear with exit status 1", "[system]") {
	TempDirectory directory;
	const auto graph = writeGraph(directory, "FX");
	ear::test::ChildProcess ear(EAR_BINARY, {"--config", graph.string(), "--control-port", "0", "--recordings-dir",
	                                         (directory.path() / "recordings").string()});
	const auto exitStatus = ear.waitForExit(deadline);
	INFO("ear printed:\n" << ear.output());
	CHECK(exitStatus == 1);
	CHECK(ear.output().find("unknown-port rec:FX") != std::string::npos);
}

TEST_CASE("A recordings folder ear cannot make stops ear with exit status 1", "[system]") {
	TempDirectory directory;
	const auto graph = writeGraph(directory, "FL");
	const auto recordings = directory.path() / "missing" / "recordings";
	ear::test::ChildProcess ear(EAR_BINARY, {"--config", graph.string(), "--control-port", "0", "--recordings-dir",
	                                         recordings.string()});
	const auto exitStatus = ear.waitForExit(deadline);
	INFO("ear printed:\n" << ear.output());
	CHECK(exitStatus == 1);
	CHECK(ear.output().find("recordings folder " + recordings.string() + ": cannot create") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(directory.path() / "missing"));
}
