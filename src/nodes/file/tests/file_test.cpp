// The "file" node type through TypeHarness (a real NodeHost over fakes), by the banks its spec declares
// (file_spec.hpp): the harness's sink hands each chunk to the type's WavWriter at once, on the test's thread, and its
// recordings are made beneath a real recordings folder, as main.cpp's adapter makes them (docs/node-types.md#file). The
// WAV files are read back with libsndfile. Paths are relative to a recordings folder and never lead out of it.
#include <sndfile.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "catch2/catch.hpp"
#include "confined_recordings.hpp"
#include "ear/folder/confined_folder.hpp"
#include "ear/nodes/file.hpp"
#include "ear/testing/type_harness.hpp"
#include "file_spec.hpp"
#include "temp_folder.hpp"

namespace {

namespace file = ear::nodes::file;
using Harness = ear::testing::TypeHarness<file::spec, file::File>;

using ear::test::TempFolder;

const ear::NodeType& fileType() {
	static const auto type = ear::nodes::makeFileNodeType();
	return *type;
}

/// The recordings folder at `path`, made if missing, behind the recording kind's adapter.
struct Recordings {
	explicit Recordings(const std::filesystem::path& path) {
		auto folder = ear::folder::ConfinedFolder::open(path);
		REQUIRE(folder.has_value());
		adapter = std::make_unique<ear::test::ConfinedRecordings>(
			std::make_shared<const ear::folder::ConfinedFolder>(std::move(*folder)));
	}
	std::unique_ptr<ear::test::ConfinedRecordings> adapter;
};

/// A harness on a node named "rec" recording to `path` beneath `recordings`, its settings parsed.
std::unique_ptr<Harness> parsed(ear::RecordingAdapter& recordings, const std::string& path,
                                ear::AudioFormat format = {.rate = 48000, .maxBlock = 256}) {
	auto harness =
		std::make_unique<Harness>(Harness::Options{.format = format, .node = "rec", .recordings = &recordings});
	REQUIRE_FALSE(harness->parse({{"path", path}}));
	return harness;
}

std::string contents(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// Whether the node asked for a sink.
bool madeSink(const Harness& harness) {
	for (const auto& companion : harness.companions())
		if (companion.kind == ear::CompanionKind::sink) return true;
	return false;
}

/// One cycle of `frames` into record.left and record.right, frame n of the graph clock being left n / 1024 and right
/// -n / 2048 - 0.25 (exact as floats); appended to `left` and `right`.
void feed(Harness& harness, std::uint64_t first, std::uint32_t frames, std::vector<float>& left,
          std::vector<float>& right) {
	const std::span<float> fl = harness.input(file::record, 0);
	const std::span<float> fr = harness.input(file::record, 1);
	for (std::uint32_t i = 0; i < frames; ++i) {
		const auto n = static_cast<float>(first + i);
		fl[i] = n / 1024.0F;
		fr[i] = -n / 2048.0F - 0.25F;
		left.push_back(fl[i]);
		right.push_back(fr[i]);
	}
	harness.process(frames);
}

/// A WAV file read back whole: its info and its interleaved samples.
struct Wav {
	SF_INFO info{};
	std::vector<float> samples;
};
Wav readWav(const std::filesystem::path& path) {
	Wav wav;
	SNDFILE* file = sf_open(path.c_str(), SFM_READ, &wav.info);
	REQUIRE(file != nullptr);
	wav.samples.resize(static_cast<std::size_t>(wav.info.frames) * static_cast<std::size_t>(wav.info.channels));
	CHECK(sf_readf_float(file, wav.samples.data(), wav.info.frames) == wav.info.frames);
	sf_close(file);
	return wav;
}

/// Every frame of `wav` is left and right, in order.
void checkFrames(const Wav& wav, const std::vector<float>& left, const std::vector<float>& right) {
	REQUIRE(wav.info.channels == 2);
	REQUIRE(static_cast<std::size_t>(wav.info.frames) == left.size());
	for (std::size_t frame = 0; frame < left.size(); ++frame) {
		INFO("frame " << frame);
		REQUIRE(wav.samples[2 * frame] == left[frame]);
		REQUIRE(wav.samples[2 * frame + 1] == right[frame]);
	}
}

/// A recording adapter whose every file is the write end of a pipe: the folder opens it, libsndfile cannot write a WAV
/// header to it (a WAV needs a seekable file).
class PipeRecordings final : public ear::RecordingAdapter {
public:
	struct PipeEnd final : ear::RecordingFile {
		explicit PipeEnd(int fd) : fd_(fd) {}
		~PipeEnd() override { ::close(fd_); }
		int descriptor() const noexcept override { return fd_; }
		int fd_;
	};
	~PipeRecordings() {
		for (const int fd : readEnds_) ::close(fd);
	}
	std::expected<std::unique_ptr<ear::RecordingFile>, std::string> open(const ear::NodeName&,
	                                                                     const std::string&) override {
		std::array<int, 2> ends{};
		REQUIRE(::pipe(ends.data()) == 0);
		readEnds_.push_back(ends[0]);
		return std::make_unique<PipeEnd>(ends[1]);
	}

private:
	std::vector<int> readEnds_;
};

} // namespace

TEST_CASE("File declares inputs record.left and record.right, no outputs, and its sink's inside bank writer",
          "[file]") {
	TempFolder dir{"ports"};
	Recordings recordings(dir.path());
	CHECK(fileType().name() == "file");
	auto harness = parsed(*recordings.adapter, "rec.wav");
	const ear::Ports& ports = harness->node().ports();
	CHECK(ports.inputs() == std::vector<ear::PortName>{"record.left", "record.right"});
	CHECK(ports.outputs().empty());
	CHECK(ports.insideOutputs() == std::vector<ear::PortName>{"writer.left", "writer.right"});
}

TEST_CASE("File settings need exactly a non-empty string path", "[file]") {
	for (const ear::NodeSettings& settings :
	     {ear::NodeSettings{}, ear::NodeSettings::object(), ear::NodeSettings{{"path", ""}},
	      ear::NodeSettings{{"path", 7}}, ear::NodeSettings{{"path", "a.wav"}, {"gain", 1}},
	      ear::NodeSettings("a.wav")}) {
		INFO(settings.dump());
		Harness harness;
		const auto refused = harness.parse(settings);
		REQUIRE(refused);
		CHECK_FALSE(refused->empty());
	}
}

TEST_CASE("File refuses an absolute path or a .. component as invalid settings", "[file]") {
	TempFolder dir{"outside"};
	for (const std::string& path :
	     {std::string("/tmp/rec.wav"), (dir.path() / "rec.wav").string(), std::string("../x.wav"), std::string(".."),
	      std::string("take/../../x.wav"), std::string("take/..")}) {
		INFO(path);
		const auto config = fileType().parse("rec", {{"path", path}});
		REQUIRE_FALSE(config.has_value());
		CHECK(config.error().node == "rec");
		CHECK(config.error().detail == R"(path: must be relative to the recordings folder, without "..")");
	}
	for (const std::string path : {"rec.wav", "take/rec.wav", "..rec.wav", "./rec.wav"}) {
		INFO(path);
		CHECK(fileType().parse("rec", {{"path", path}}).has_value());
	}
}

TEST_CASE("File fails its configure on a symlink in the folder that leads outside it, and asks for no sink", "[file]") {
	TempFolder dir{"symlink"};
	const auto outside = dir.path() / "outside.wav";
	std::ofstream(outside, std::ios::binary) << "keep";
	Recordings recordings(dir.path() / "recordings");
	std::filesystem::create_symlink(outside, dir.path() / "recordings" / "rec.wav");
	std::filesystem::create_directory_symlink(dir.path(), dir.path() / "recordings" / "up");

	for (const std::string path : {"rec.wav", "up/outside.wav", "up/new.wav"}) {
		INFO(path);
		auto harness = parsed(*recordings.adapter, path);

		harness->add();

		CHECK(harness->phase() == "failed");
		REQUIRE(harness->health().reason);
		CHECK(harness->health().reason->code == ear::FailureReason::Code::startFailed);
		CHECK(harness->health().reason->detail == "cannot open " + path + ": it leads outside the folder");
		CHECK_FALSE(madeSink(*harness));
	}
	CHECK(contents(outside) == "keep");
	CHECK_FALSE(std::filesystem::exists(dir.path() / "new.wav"));
}

TEST_CASE("File records record.left and record.right through its sink as a 32-bit float stereo WAV at the format's "
          "rate",
          "[file]") {
	TempFolder dir{"records"};
	const auto wav = dir.path() / "rec.wav";
	{
		// Longer than what the node writes: the node must truncate it.
		std::ofstream old{wav, std::ios::binary};
		old << std::string(1 << 20, 'x');
	}
	constexpr ear::AudioFormat format{.rate = 32000, .maxBlock = 256};
	constexpr std::uint32_t blocks = 3;
	Recordings recordings(dir.path());
	auto harness = parsed(*recordings.adapter, "rec.wav", format);

	harness->add();
	harness->blockReady();
	CHECK(harness->phase() == "running");
	REQUIRE(harness->recordingRequests().size() == 1);
	CHECK(harness->recordingRequests()[0].path == "rec.wav");
	ear::testing::CompanionFake& sink = harness->companion(file::writer);
	CHECK(sink.kind == ear::CompanionKind::sink);
	CHECK(sink.name == "rec.writer");
	CHECK(std::get<ear::SinkShape>(sink.shape).frames == 2 * format.rate); // 2 s ring
	CHECK(sink.ready);                                                     // its consumer began: the WAV is open
	CHECK(harness->handoffs().empty());

	std::vector<float> left;
	std::vector<float> right;
	for (std::uint32_t block = 0; block < blocks; ++block)
		feed(*harness, std::uint64_t{block} * format.maxBlock, format.maxBlock, left, right);
	CHECK(harness->unwritten() == 0);
	REQUIRE(sink.chunks.size() == blocks);
	CHECK(sink.chunks[0].frames == format.maxBlock);
	// The plan carries latency from `record` to the sink's bank and adds none of file's own, so a chunk's upstreamFrames
	// is what lies upstream of `record` (docs/engine.md#sinks).
	CHECK((*harness)->latencyPlan().pairs == std::vector<ear::BankPair>{{"record", "writer"}});
	CHECK((*harness)->latencyPlan().internal.empty());
	CHECK((*harness)->latencyPlan().beyond.empty());
	harness->remove(); // the sink's close finishes the WAV
	CHECK(harness->gone());
	CHECK(harness->droppedFrames() == 0);

	const Wav read = readWav(wav);
	CHECK(read.info.samplerate == static_cast<int>(format.rate));
	CHECK(read.info.format == (SF_FORMAT_WAV | SF_FORMAT_FLOAT));
	checkFrames(read, left, right);
}

TEST_CASE("File: every cycle the code drove reaches the sink, none after the stop's position and none before the "
          "start's",
          "[file]") {
	TempFolder dir{"stop"};
	Recordings recordings(dir.path());
	auto harness = parsed(*recordings.adapter, "rec.wav");
	constexpr std::uint32_t kBlock = 256;
	harness->add();
	harness->blockReady();
	std::vector<float> left;
	std::vector<float> right;
	std::vector<float> unheardLeft;
	std::vector<float> unheardRight;
	feed(*harness, 0, kBlock, left, right);
	feed(*harness, kBlock, kBlock, left, right);
	REQUIRE(harness->stop());
	CHECK(harness->phase() == "stopped");
	feed(*harness, 2 * kBlock, kBlock, unheardLeft, unheardRight); // stopped: the block is silent and the sink unbound
	feed(*harness, 3 * kBlock, kBlock, unheardLeft, unheardRight);
	REQUIRE(harness->start());
	CHECK(harness->phase() == "running");
	feed(*harness, 4 * kBlock, kBlock, left, right);
	feed(*harness, 5 * kBlock, kBlock, left, right);

	const ear::testing::CompanionFake& sink = harness->companion(file::writer);
	CHECK_FALSE(sink.destroyed); // a stop closes nothing
	std::vector<std::uint64_t> positions;
	for (const auto& chunk : sink.chunks) positions.push_back(chunk.stamp.position);
	CHECK(positions == std::vector<std::uint64_t>{0, kBlock, 4 * kBlock, 5 * kBlock});
	CHECK(harness->companions().size() == 1); // one sink, bound again by the start
	harness->remove();
	checkFrames(readWav(dir.path() / "rec.wav"), left, right);
}

TEST_CASE("File: a path it cannot open is Configured::failed and asks for no sink", "[file]") {
	TempFolder dir{"unopenable"};
	Recordings recordings(dir.path());
	auto harness = parsed(*recordings.adapter, "missing-dir/rec.wav");

	harness->add();

	CHECK(harness->phase() == "failed");
	REQUIRE(harness->health().reason);
	CHECK(harness->health().reason->code == ear::FailureReason::Code::startFailed);
	CHECK(harness->health().reason->detail.starts_with("cannot open missing-dir/rec.wav: "));
	CHECK_FALSE(madeSink(*harness));
}

TEST_CASE("File fails its configure when the host has no recordings folder", "[file]") {
	Harness harness;
	REQUIRE_FALSE(harness.parse({{"path", "rec.wav"}}));
	harness.add();
	REQUIRE(harness->health().reason);
	CHECK(harness->health().reason->detail == "cannot open rec.wav: no recordings folder");
	CHECK_FALSE(madeSink(harness));
}

TEST_CASE("File: a WAV libsndfile cannot open fails the node start-failed from its sink's begin", "[file]") {
	PipeRecordings pipes;
	auto harness = parsed(pipes, "rec.wav");

	harness->add();

	// The sink was made and its consumer's begin said lost before `begun`: the node never reached configured.
	CHECK(madeSink(*harness));
	CHECK(harness->phase() == "failed");
	REQUIRE(harness->health().reason);
	CHECK(harness->health().reason->code == ear::FailureReason::Code::startFailed);
	CHECK(harness->health().reason->detail.starts_with("cannot open rec.wav: "));
	CHECK(harness->companion(file::writer).destroyed); // closed, with nothing to finish but a file never opened
}

TEST_CASE("File: a restart finishes the old WAV and opens the new one", "[file]") {
	TempFolder dir{"restart"};
	Recordings recordings(dir.path());
	auto harness = parsed(*recordings.adapter, "a.wav");
	constexpr std::uint32_t kBlock = 256;
	harness->add();
	harness->blockReady();
	std::vector<float> leftA;
	std::vector<float> rightA;
	feed(*harness, 0, kBlock, leftA, rightA);
	feed(*harness, kBlock, kBlock, leftA, rightA);

	CHECK(harness->change({{"path", "b.wav"}}) == ear::NodeOutcome::restart);
	CHECK(harness->phase() == "running");
	// The old configuration's sink closed: a.wav is complete before b.wav takes a frame.
	REQUIRE(harness->companions().size() == 2);
	CHECK(harness->companions()[0].destroyed);
	checkFrames(readWav(dir.path() / "a.wav"), leftA, rightA);
	REQUIRE(harness->recordingRequests().size() == 2);
	CHECK(harness->recordingRequests()[1].path == "b.wav");

	std::vector<float> leftB;
	std::vector<float> rightB;
	feed(*harness, 2 * kBlock, kBlock, leftB, rightB);
	harness->remove();
	checkFrames(readWav(dir.path() / "b.wav"), leftB, rightB);
	CHECK(harness->droppedFrames() == 0);
}
