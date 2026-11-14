// The "file" node type through TypeHarness (a real NodeHost over fakes), by the banks its spec declares
// (file_spec.hpp): the harness's sink hands each chunk to the type's WavWriter at once, on the test's thread, and its
// recordings are the harness's in-memory files, claimed, opened and closed by the framework on the harness's I/O
// executor (docs/node-types.md#file, docs/writing-a-node-type.md#recordings). The WAV files are read back with
// libsndfile. That a path never leads out of the recordings folder is the folder's (ear_folder's and ear_runtime's tests).
#include <sndfile.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "catch2/catch.hpp"
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

/// A harness on a node named "rec" recording to `path`, its settings parsed.
std::unique_ptr<Harness> parsed(const std::string& path, Harness::Options options = {}) {
	options.node = "rec";
	auto harness = std::make_unique<Harness>(std::move(options));
	REQUIRE_FALSE(harness->parse({{"path", path}}));
	return harness;
}

/// Whether the node asked for a sink.
bool madeSink(const Harness& harness) {
	return std::ranges::any_of(harness.companions(),
	                           [](const auto& companion) { return companion.kind == ear::CompanionKind::sink; });
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
/// The recording at `path` on the harness's disk, read through a temporary file.
Wav readWav(const Harness& harness, const std::string& path) {
	const auto bytes = harness.recordingBytes(path);
	REQUIRE(bytes);
	const TempFolder dir{"wav"};
	const auto copy = dir.path() / "read.wav";
	std::ofstream(copy, std::ios::binary) << *bytes;
	Wav wav;
	SNDFILE* opened = sf_open(copy.c_str(), SFM_READ, &wav.info);
	REQUIRE(opened != nullptr);
	wav.samples.resize(static_cast<std::size_t>(wav.info.frames) * static_cast<std::size_t>(wav.info.channels));
	CHECK(sf_readf_float(opened, wav.samples.data(), wav.info.frames) == wav.info.frames);
	sf_close(opened);
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

/// The index of `job` in what the harness's I/O executor ran (its size when absent).
std::size_t ranAt(Harness& harness, const std::string& job) {
	const auto& jobs = harness->ioJobs();
	return static_cast<std::size_t>(std::ranges::find(jobs, job) - jobs.begin());
}

} // namespace

TEST_CASE("File declares inputs record.left and record.right, no outputs, and its sink's inside bank writer",
          "[file]") {
	CHECK(fileType().name() == "file");
	auto harness = parsed("rec.wav");
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

TEST_CASE("File records record.left and record.right through its sink as a 32-bit float stereo WAV at the format's "
          "rate",
          "[file]") {
	constexpr ear::AudioFormat format{.rate = ear::SampleRate::fixed<32000>(), .maxBlock = 256};
	constexpr std::uint32_t blocks = 3;
	auto harness = parsed("rec.wav", {.format = format});
	harness->presetRecording("rec.wav", std::string(1 << 20, 'x')); // longer than what the node writes: truncated

	harness->add();
	harness->blockReady();
	CHECK(harness->phase() == "running");
	REQUIRE(harness->recordingRequests().size() == 1);
	CHECK(harness->recordingRequests()[0].path == "rec.wav");
	ear::testing::CompanionFake& sink = harness->companion(file::writer);
	CHECK(sink.kind == ear::CompanionKind::sink);
	CHECK(sink.name == "rec.writer");
	CHECK(std::get<ear::SinkShape>(sink.shape).frames == 2 * format.rate.hz()); // 2 s ring
	CHECK(sink.ready);                                                          // its consumer began: the WAV is open
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
	CHECK((*harness)->latencyPlan().onPair.empty());
	CHECK((*harness)->latencyPlan().onBank.empty());
	CHECK((*harness)->latencyPlan().beyond.empty());
	harness->remove(); // the sink's close finishes the WAV
	CHECK(harness->gone());
	CHECK(harness->droppedFrames() == 0);

	const Wav read = readWav(*harness, "rec.wav");
	CHECK(read.info.samplerate == static_cast<int>(format.rate.hz()));
	CHECK(read.info.format == (SF_FORMAT_WAV | SF_FORMAT_FLOAT));
	checkFrames(read, left, right);
}

TEST_CASE("File: its recording is opened on the I/O thread before its sink's consumer begins, and closed there after "
          "its sink finished",
          "[file][recording]") {
	auto harness = parsed("rec.wav", {.holdIo = true});
	harness->add();
	harness->blockReady();
	CHECK(harness->phase() == "configuring");
	CHECK_FALSE(harness->companion(file::writer).ready); // nothing begins on a file not opened yet
	CHECK_FALSE(harness->recordingBytes("rec.wav"));
	harness->node().advance(ear::waitingForDelay);
	CHECK(harness->health().waitingFor == std::vector<ear::WaitingFor>{{ear::WaitingFor::Kind::recording, "rec.wav"},
	                                                                   {ear::WaitingFor::Kind::consumer, ""}});
	harness->runIo();
	CHECK(harness->phase() == "running");
	CHECK(harness->companion(file::writer).ready);
	CHECK(harness->openRecordings() == 1);
	CHECK(ranAt(*harness, "open recording rec.wav") == 0);

	harness->remove();
	CHECK(harness->companion(file::writer).destroyed); // the sink finished the WAV
	CHECK(harness->openRecordings() == 1);             // its close waits for the I/O thread
	harness->runIo();
	CHECK(harness->openRecordings() == 0);
	CHECK(ranAt(*harness, "close recording rec.wav") == 1);
	CHECK(readWav(*harness, "rec.wav").info.frames == 0);
}

TEST_CASE("File: an open past its 5 s bound fails the node start-failed, and the late open is closed on the I/O thread",
          "[file][recording][deadline]") {
	auto harness = parsed("rec.wav", {.holdIo = true});
	harness->add();
	harness->blockReady();
	harness->node().advance(ear::recordingBound - std::chrono::milliseconds{1});
	CHECK(harness->phase() == "configuring");
	harness->node().advance(std::chrono::milliseconds{1});
	CHECK(harness->phase() == "failed");
	REQUIRE(harness->health().reason);
	CHECK(harness->health().reason->code == ear::FailureReason::Code::startFailed);
	CHECK(harness->health().reason->detail == "cannot open rec.wav: not opened within 5 s");
	CHECK_FALSE(harness->companion(file::writer).ready); // its consumer never began
	harness->runIo(); // the open runs at last: its completion lands on no configuration, its file released
	CHECK(harness->openRecordings() == 0);
	CHECK(ranAt(*harness, "release recording rec.wav") < (*harness)->ioJobs().size());
}

TEST_CASE("File: a path the folder refuses fails the node start-failed at its open, and its consumer never begins",
          "[file]") {
	auto harness = parsed("missing-dir/rec.wav");
	harness->failRecordingOpen("missing-dir/rec.wav", "No such file or directory");

	harness->add();

	CHECK(harness->phase() == "failed");
	REQUIRE(harness->health().reason);
	CHECK(harness->health().reason->code == ear::FailureReason::Code::startFailed);
	CHECK(harness->health().reason->detail == "cannot open missing-dir/rec.wav: No such file or directory");
	REQUIRE(madeSink(*harness));
	CHECK_FALSE(harness->companion(file::writer).ready);
	CHECK(harness->companion(file::writer).destroyed); // closed with nothing begun
	CHECK(harness->openRecordings() == 0);
}

TEST_CASE("File fails its configure when the host has no recordings folder", "[file]") {
	auto harness = parsed("rec.wav", {.noRecordingsFolder = true});
	harness->add();
	REQUIRE(harness->health().reason);
	CHECK(harness->health().reason->code == ear::FailureReason::Code::startFailed);
	CHECK(harness->health().reason->detail == "cannot open rec.wav: no recordings folder");
	CHECK((*harness)->ioJobs().empty());
}

TEST_CASE("File: a WAV libsndfile cannot open fails the node start-failed from its sink's begin", "[file]") {
	auto harness = parsed("rec.wav");
	harness->pipeRecording("rec.wav");

	harness->add();

	// The sink was made and its consumer's begin said lost before `begun`: the node never reached configured.
	CHECK(madeSink(*harness));
	CHECK(harness->phase() == "failed");
	REQUIRE(harness->health().reason);
	CHECK(harness->health().reason->code == ear::FailureReason::Code::startFailed);
	CHECK(harness->health().reason->detail.starts_with("cannot open rec.wav: "));
	CHECK(harness->companion(file::writer).destroyed); // closed, with nothing to finish but a file never opened
}

TEST_CASE("File: every cycle the code drove reaches the sink, none after the stop's position and none before the "
          "start's",
          "[file]") {
	auto harness = parsed("rec.wav");
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
	CHECK(harness->openRecordings() == 1);
	std::vector<std::uint64_t> positions;
	for (const auto& chunk : sink.chunks) positions.push_back(chunk.stamp.position);
	CHECK(positions == std::vector<std::uint64_t>{0, kBlock, 4 * kBlock, 5 * kBlock});
	CHECK(harness->companions().size() == 1); // one sink, bound again by the start
	harness->remove();
	checkFrames(readWav(*harness, "rec.wav"), left, right);
}

TEST_CASE("File writes every frame in order when its sink hands chunks split at the ring's end", "[file]") {
	// A 256-frame ring and 100-frame cycles: the sink hands most cycles in two calls, as the engine's does
	// (docs/engine.md#sinks).
	auto harness = parsed("rec.wav", {.sinkRingFrames = 256});
	constexpr std::uint32_t kBlock = 100;
	constexpr std::uint32_t kBlocks = 10;
	harness->add();
	harness->blockReady();
	std::vector<float> left;
	std::vector<float> right;
	for (std::uint32_t block = 0; block < kBlocks; ++block)
		feed(*harness, std::uint64_t{block} * kBlock, kBlock, left, right);
	const ear::testing::CompanionFake& sink = harness->companion(file::writer);
	CHECK(sink.chunks.size() > kBlocks); // some cycles were split
	std::uint64_t next = 0;
	for (const auto& chunk : sink.chunks) {
		CHECK(chunk.stamp.position == next); // each call dated by its own first frame
		next += chunk.frames;
	}
	harness->remove();
	CHECK(harness->droppedFrames() == 0);
	checkFrames(readWav(*harness, "rec.wav"), left, right);
}

TEST_CASE("File: a restart finishes the old WAV and opens the new one", "[file]") {
	auto harness = parsed("a.wav");
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
	checkFrames(readWav(*harness, "a.wav"), leftA, rightA);
	REQUIRE(harness->recordingRequests().size() == 2);
	CHECK(harness->recordingRequests()[1].path == "b.wav");
	CHECK(ranAt(*harness, "close recording a.wav") < ranAt(*harness, "open recording b.wav"));

	std::vector<float> leftB;
	std::vector<float> rightB;
	feed(*harness, 2 * kBlock, kBlock, leftB, rightB);
	harness->remove();
	checkFrames(readWav(*harness, "b.wav"), leftB, rightB);
	CHECK(harness->droppedFrames() == 0);
}

TEST_CASE("File: a restart on the same path truncates it and records afresh, its old file closed first", "[file]") {
	auto harness = parsed("rec.wav");
	constexpr std::uint32_t kBlock = 256;
	harness->add();
	harness->blockReady();
	std::vector<float> leftA;
	std::vector<float> rightA;
	feed(*harness, 0, kBlock, leftA, rightA);
	feed(*harness, kBlock, kBlock, leftA, rightA);
	REQUIRE(harness->restart());
	CHECK(harness->phase() == "running"); // its own claim was released at its teardown's last step
	CHECK(ranAt(*harness, "close recording rec.wav") < (*harness)->ioJobs().size());
	std::vector<float> leftB;
	std::vector<float> rightB;
	feed(*harness, 2 * kBlock, kBlock, leftB, rightB);
	harness->remove();
	checkFrames(readWav(*harness, "rec.wav"), leftB, rightB);
}

// docs/engine.md#readiness-deadlines, on the harness's clock: with holdSinks the consumer begins only at ready(bank).
TEST_CASE("File: a sink whose consumer has not begun within 5 s fails the node start-failed, and one that begins in "
          "time runs",
          "[file][deadline]") {
	auto harness = parsed("rec.wav", {.holdSinks = true});
	harness->add();
	harness->blockReady();
	CHECK(harness->phase() == "configuring");
	CHECK_FALSE(harness->companion(file::writer).ready);
	// Its list as a client will see it: the sink, not ready, nothing dropped.
	CHECK(
		harness->companionList() ==
		std::vector<ear::CompanionView>{
			{.name = "rec.writer", .kind = ear::CompanionKind::sink, .bank = "writer", .ready = false, .dropped = 0}});
	harness->node().advance(std::chrono::milliseconds(4'999));
	CHECK(harness->phase() == "configuring");
	SECTION("its consumer begins in time") {
		harness->ready(file::writer);
		CHECK(harness->companion(file::writer).ready);
		CHECK(harness->phase() == "running");
		REQUIRE(harness->companionList().size() == 1);
		CHECK(harness->companionList()[0].ready);
		harness->node().advance(std::chrono::milliseconds(10'000));
		CHECK(harness->phase() == "running");
	}
	SECTION("its consumer never begins") {
		harness->node().advance(std::chrono::milliseconds(1));
		CHECK(harness->phase() == "failed");
		REQUIRE(harness->health().reason);
		CHECK(harness->health().reason->code == ear::FailureReason::Code::startFailed);
		CHECK(harness->health().reason->detail == "the sink writer's consumer did not begin within 5 s");
		CHECK(harness->companion(file::writer).destroyed);
		CHECK(harness->companionList().empty()); // torn down
	}
	// A sink is never lost by the framework.
	CHECK_THROWS_AS(harness->node().lose("writer", "x"), std::logic_error);
}
