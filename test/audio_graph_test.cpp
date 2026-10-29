// Integration test: EAR's audio graph between virtual sources and a virtual output device.
//
// No OS audio device is opened, started or enumerated. The graph is built by the function
// run() in src/server.hpp uses, onto a VirtualAudioDevice that the test thread clocks one block
// at a time from a separate audio thread. The AirPlay source reads real PCM from a named pipe written by the test.
//
// This is the only translation unit that includes the ear headers (pipe_source.hpp defines a
// non-inline member function).

#include "catch2/catch.hpp"

#include "virtual_audio_device.hpp"

#include "ear/graph_config.hpp"

#include "juce_dsp/juce_dsp.h"
#include "nlohmann/json.hpp"

#include <boost/uuid/uuid_io.hpp>

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using ear::test::VirtualAudioDevice;
using ear::test::VirtualAudioDeviceType;

constexpr int OUTPUT_CHANNELS = 4;
constexpr int BLOCK = VirtualAudioDevice::BLOCK_SIZE;
constexpr double RATE = VirtualAudioDevice::SAMPLE_RATE;

// ---------------------------------------------------------------------------------------------
// The graph under test, built from a config by run()'s own graph builder.

class GraphRig {
public:
	explicit GraphRig(const nlohmann::json& config)
		: _type(std::make_shared<VirtualAudioDeviceType>(OUTPUT_CHANNELS))
		, _device(std::make_shared<ear::AudioIoDevice>(_type, VirtualAudioDeviceType::DEVICE_NAME))
		, _graph(std::make_unique<ear::AudioGraphStandalone>())
	{
		// run() in src/server.hpp builds its graph with this same function.
		_error = ear::buildGraphFromConfig(*_graph, _device, config);

		for (auto* node : _graph->getNodes()) {
			if (auto* source = dynamic_cast<ear::GraphSource*>(node->getProcessor())) {
				_sources.push_back(source);
			}
		}
		if (config.count("nodes")) {
			for (auto& nodeConfig : config["nodes"]) {
				if (nodeConfig["type"] == "airplay") {
					_pipes.push_back(nodeConfig["pipe"].get<std::string>());
				}
			}
		}

		_opened = _device->open();
		if (_opened) {
			_device->start();
		}
	}

	~GraphRig() {
		// Stop the device first: ear::AudioIoDevice::removeCallback loops forever if it runs
		// while the device is started.
		_device->stop();

		// Release any AirPlay reader still blocked opening its pipe (opening a FIFO for reading
		// blocks until a writer appears), so stopping it below cannot hang.
		for (const auto& pipe : _pipes) {
			const int fd = ::open(pipe.c_str(), O_WRONLY | O_NONBLOCK);
			if (fd >= 0) {
				::close(fd);
			}
		}

		// Stop the sources' reader threads. Nothing in production does this: GraphSource does not
		// forward releaseResources() to its player, and destroying an AirplayProcessor whose
		// reader thread runs destroys a joinable std::thread (std::terminate).
		for (auto* source : _sources) {
			source->_player.setSource(nullptr);
		}

		_graph.reset();
		_device.reset();
	}

	// What buildGraphFromConfig reported; empty when the graph was built.
	const std::string& buildError() const { return _error; }

	bool started() const { return _opened && virtualDevice().isPlaying(); }

	VirtualAudioDevice& virtualDevice() const {
		auto* device = dynamic_cast<VirtualAudioDevice*>(_device->getDevice().get());
		assert(device != nullptr);
		return *device;
	}

private:
	std::shared_ptr<juce::AudioIODeviceType> _type;
	std::shared_ptr<ear::AudioIoDevice> _device;
	std::unique_ptr<ear::AudioGraphStandalone> _graph;
	std::vector<ear::GraphSource*> _sources;
	std::vector<std::string> _pipes;
	std::string _error;
	bool _opened{false};
};

// ---------------------------------------------------------------------------------------------
// The AirPlay side: a named pipe in a fresh temp directory and the PCM written into it.

class TempFifo {
public:
	// mkdtemp creates the directory atomically, so test processes run in parallel
	// (ctest -j) never share a pipe.
	TempFifo() {
		std::string pattern = juce::File::getSpecialLocation(juce::File::tempDirectory)
			.getChildFile("ear-test-XXXXXX").getFullPathName().toStdString();
		if (::mkdtemp(pattern.data()) == nullptr) {
			return;
		}
		_dir = juce::File(pattern);
		_path = _dir.getChildFile("airplay").getFullPathName().toStdString();
		_created = ::mkfifo(_path.c_str(), 0600) == 0;
	}

	~TempFifo() {
		if (_dir != juce::File()) {
			_dir.deleteRecursively();
		}
	}

	bool created() const { return _created; }

	const std::string& path() const { return _path; }

private:
	juce::File _dir;
	std::string _path;
	bool _created{false};
};

class UniqueFd {
public:
	explicit UniqueFd(int fd = -1) : _fd(fd) {}
	UniqueFd(UniqueFd&& other) noexcept : _fd(std::exchange(other._fd, -1)) {}
	UniqueFd& operator=(UniqueFd&& other) noexcept { reset(std::exchange(other._fd, -1)); return *this; }
	~UniqueFd() { reset(); }

	int get() const { return _fd; }

	void reset(int fd = -1) {
		if (_fd >= 0) {
			::close(_fd);
		}
		_fd = fd;
	}

private:
	int _fd{-1};
};

// Opens the pipe for writing once the AirPlay reader has opened it for reading. The reader
// gives no event for that, so this retries a non-blocking open (ENXIO until a reader exists)
// against a bounded deadline. Returns a blocking write descriptor, or -1 on timeout.
UniqueFd openPipeForWriting(const std::string& path) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

	while (std::chrono::steady_clock::now() < deadline) {
		const int fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
		if (fd >= 0) {
			::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
			return UniqueFd(fd);
		}
		if (errno != ENXIO) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	return UniqueFd();
}

// Writes `bytes` into the pipe from its own thread; finish() joins it.
class PipeWriter {
public:
	PipeWriter(const std::string& path, const std::vector<uint8_t>& bytes)
		: _thread([this, path, &bytes] { run(path, bytes); })
	{}

	~PipeWriter() { finish(); }

	// Waits for the writer thread to end; true if every byte was written.
	bool finish() {
		if (_thread.joinable()) {
			_thread.join();
		}
		return _complete;
	}

private:
	void run(const std::string& path, const std::vector<uint8_t>& bytes) {
		UniqueFd fd = openPipeForWriting(path);
		if (fd.get() < 0) {
			return;
		}

		// Write in PIPE_BUF chunks, which a pipe takes atomically, so every read the AirPlay
		// reader makes holds whole 4-byte stereo frames (AirplaySource asserts that).
		size_t offset = 0;
		while (offset < bytes.size()) {
			const size_t chunk = std::min<size_t>(PIPE_BUF, bytes.size() - offset);
			const ssize_t written = ::write(fd.get(), bytes.data() + offset, chunk);
			if (written <= 0) {
				return;
			}
			offset += (size_t) written;
		}

		_complete = true;
	}

	bool _complete{false};
	std::thread _thread;
};

// Interleaved stereo 16-bit little-endian PCM: one sine per channel.
struct StereoPcm {
	std::vector<int16_t> left;
	std::vector<int16_t> right;
	std::vector<uint8_t> bytes;
};

StereoPcm makeStereoSine(int frames, double leftHz, double rightHz, double amplitude) {
	StereoPcm pcm;
	pcm.bytes.reserve((size_t) frames * 4);

	const auto put = [&pcm](int16_t sample) {
		const auto value = (uint16_t) sample;
		pcm.bytes.push_back((uint8_t) (value & 0xff));
		pcm.bytes.push_back((uint8_t) (value >> 8));
	};

	for (int frame = 0; frame < frames; ++frame) {
		const double t = frame / RATE;
		const auto l = (int16_t) std::lround(amplitude * 32767.0 * std::sin(juce::MathConstants<double>::twoPi * leftHz * t));
		const auto r = (int16_t) std::lround(amplitude * 32767.0 * std::sin(juce::MathConstants<double>::twoPi * rightHz * t));
		pcm.left.push_back(l);
		pcm.right.push_back(r);
		put(l);
		put(r);
	}

	return pcm;
}

// ---------------------------------------------------------------------------------------------
// Signal measurements.

double rms(const float* samples, size_t count) {
	double sum = 0;
	for (size_t i = 0; i < count; ++i) {
		sum += (double) samples[i] * samples[i];
	}
	return count == 0 ? 0 : std::sqrt(sum / (double) count);
}

// Frequency of the strongest FFT bin over 2^order samples (Hann window).
double dominantFrequency(const float* samples, int order) {
	const int size = 1 << order;
	juce::dsp::FFT fft(order);
	juce::dsp::WindowingFunction<float> window((size_t) size, juce::dsp::WindowingFunction<float>::hann, false);

	std::vector<float> data((size_t) size * 2, 0.0f);
	std::copy(samples, samples + size, data.begin());
	window.multiplyWithWindowingTable(data.data(), (size_t) size);
	fft.performFrequencyOnlyForwardTransform(data.data());

	int peak = 1;
	for (int bin = 1; bin < size / 2; ++bin) {
		if (data[(size_t) bin] > data[(size_t) peak]) {
			peak = bin;
		}
	}
	return peak * RATE / size;
}

bool allExactlyZero(const std::vector<float>& samples) {
	return std::all_of(samples.begin(), samples.end(), [](float s) { return s == 0.0f; });
}

nlohmann::json airplayConfig(const std::string& pipe, unsigned buffer, nlohmann::json connect) {
	return {
		{"airplay", {{"buffer", buffer}}},
		{"nodes", nlohmann::json::array({
			{{"type", "airplay"}, {"pipe", pipe}, {"connect", std::move(connect)}},
		})},
	};
}

constexpr double TONE_AMPLITUDE = 0.5;
constexpr double LEFT_HZ = 440;
constexpr double RIGHT_HZ = 880;
constexpr int FFT_ORDER = 13;                 // 8192 samples, 5.4 Hz per bin
constexpr int ANALYSIS_BLOCKS = (1 << FFT_ORDER) / BLOCK;

} // namespace

TEST_CASE("AirPlay PCM from the pipe reaches only its mapped device channels", "[airplay]") {
	TempFifo fifo;
	REQUIRE(fifo.created());

	// AirPlay left -> device 1, right -> device 3; device 0 and 2 are unmapped.
	// A 200-block ring holds all 64 blocks written below, so nothing overruns while the test
	// has not started rendering yet.
	GraphRig rig(airplayConfig(fifo.path(), 200, {{0, 1}, {1, 3}}));
	REQUIRE(rig.buildError() == "");
	REQUIRE(rig.started());

	const int frames = 64 * BLOCK;
	const StereoPcm pcm = makeStereoSine(frames, LEFT_HZ, RIGHT_HZ, TONE_AMPLITUDE);

	// Event: the writer thread has handed every byte to the pipe. The pipe then holds at most
	// 64 KiB (16384 frames) the reader has not taken, so at least the first 30 blocks are in the
	// AirPlay ring before the first render and the analysed 16 blocks play without a gap.
	{
		PipeWriter writer(fifo.path(), pcm.bytes);
		REQUIRE(writer.finish());
	}

	VirtualAudioDevice& device = rig.virtualDevice();

	// The device sink copies the previous block's graph output, so the tone appears a block
	// late. Render until it appears, bounded.
	int startBlock = -1;
	for (int block = 0; block < 8 && startBlock < 0; ++block) {
		REQUIRE(device.renderBlocks(1));
		const auto& left = device.recorded(1);
		if (std::any_of(left.end() - BLOCK, left.end(), [](float s) { return s != 0.0f; })) {
			startBlock = block;
		}
	}
	REQUIRE(startBlock >= 0);
	REQUIRE(device.renderBlocks(ANALYSIS_BLOCKS - 1));

	const size_t start = (size_t) startBlock * BLOCK;
	const size_t count = (size_t) ANALYSIS_BLOCKS * BLOCK;

	SECTION("each mapped channel carries its tone at full scale") {
		const float* left = device.recorded(1).data() + start;
		const float* right = device.recorded(3).data() + start;

		const double expectedRms = TONE_AMPLITUDE / std::sqrt(2.0);
		CHECK(rms(left, count) == Approx(expectedRms).epsilon(0.02));
		CHECK(rms(right, count) == Approx(expectedRms).epsilon(0.02));

		const double binHz = RATE / (1 << FFT_ORDER);
		CHECK(dominantFrequency(left, FFT_ORDER) == Approx(LEFT_HZ).margin(binHz));
		CHECK(dominantFrequency(right, FFT_ORDER) == Approx(RIGHT_HZ).margin(binHz));
	}

	SECTION("the samples are the written PCM, frame for frame") {
		const float* left = device.recorded(1).data() + start;
		const float* right = device.recorded(3).data() + start;

		size_t mismatches = 0;
		for (size_t i = 0; i < count; ++i) {
			mismatches += left[i] != Approx(pcm.left[i] / 32767.0f).margin(1e-6);
			mismatches += right[i] != Approx(pcm.right[i] / 32767.0f).margin(1e-6);
		}
		CHECK(mismatches == 0);
	}

	SECTION("unmapped channels are exactly silent") {
		CHECK(allExactlyZero(device.recorded(0)));
		CHECK(allExactlyZero(device.recorded(2)));
	}
}

TEST_CASE("AirPlay with no data in the pipe renders silence", "[airplay]") {
	TempFifo fifo;
	REQUIRE(fifo.created());

	GraphRig rig(airplayConfig(fifo.path(), 40, {{0, 0}, {1, 1}}));
	REQUIRE(rig.buildError() == "");
	REQUIRE(rig.started());

	// A connected but idle writer, like shairport-sync between tracks. Declared after the rig,
	// so it closes first and the reader thread can stop.
	UniqueFd writer = openPipeForWriting(fifo.path());
	REQUIRE(writer.get() >= 0);

	VirtualAudioDevice& device = rig.virtualDevice();
	REQUIRE(device.renderBlocks(32));

	for (int channel = 0; channel < OUTPUT_CHANNELS; ++channel) {
		INFO("device channel " << channel);
		CHECK(allExactlyZero(device.recorded(channel)));
	}
}

TEST_CASE("White noise reaches only its mapped device channel", "[white]") {
	const nlohmann::json config = {
		{"nodes", nlohmann::json::array({
			{{"type", "white"}, {"connect", {{0, 2}}}},
		})},
	};

	GraphRig rig(config);
	REQUIRE(rig.buildError() == "");
	REQUIRE(rig.started());

	VirtualAudioDevice& device = rig.virtualDevice();
	REQUIRE(device.renderBlocks(17));

	// Skip the first block, which the device sink fills before the graph's first render.
	const auto& noise = device.recorded(2);
	const double noiseRms = rms(noise.data() + BLOCK, noise.size() - BLOCK);
	CHECK(noiseRms == Approx(1.0 / std::sqrt(3.0)).epsilon(0.05)); // uniform in [-1, 1)

	for (int channel : {0, 1, 3}) {
		INFO("device channel " << channel);
		CHECK(allExactlyZero(device.recorded(channel)));
	}
}

TEST_CASE("A device's UUID is the name-based UUID of its type and name", "[device]") {
	std::shared_ptr<juce::AudioIODeviceType> type = std::make_shared<VirtualAudioDeviceType>(OUTPUT_CHANNELS);
	const ear::AudioIoDevice device(type, VirtualAudioDeviceType::DEVICE_NAME);

	// Users pass this value with --device and in the config's "device": it must not change.
	// Python: uuid.uuid5(uuid.UUID(int=0), "Virtual:Virtual Output")
	CHECK(boost::uuids::to_string(device.getUuid()) == "a3a610c8-d8b9-588d-b05e-5335729dde2b");
}
