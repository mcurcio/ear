// The "file" node type through the node harness (a real NodeHost over fakes), by the bank its spec declares
// (file_spec.hpp), whose synchronous handoff stands in for the framework's and whose recordings are made beneath a real
// recordings folder, as main.cpp's adapter makes them (docs/design/m1-white-to-file.md section 3, slice S5 criteria 2
// and 3; docs/design/node-framework.md sections 7.3 and 7.6). Until step L-4b `file` is a legacy type
// (makeLegacyNodeType), so it runs through NodeHarness by names, not TypeHarness. The WAV files are read back with
// libsndfile. Paths are relative to a recordings folder and never lead out of it (docs/design/batch2.md section 4.4).
#include <sndfile.h>

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
#include "ear/testing/node_harness.hpp"
#include "file_spec.hpp"
#include "temp_folder.hpp"

namespace {

namespace file = ear::nodes::file;
using Harness = ear::testing::NodeHarness;

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

/// A harness on a node named "rec" recording to `path`, its settings parsed.
std::unique_ptr<Harness> parsed(Recordings& recordings, const std::string& path,
                                ear::AudioFormat format = {.rate = 48000, .maxBlock = 256}) {
	auto harness = std::make_unique<Harness>(
		fileType(), Harness::Options{.format = format, .recordings = recordings.adapter.get()});
	REQUIRE_FALSE(harness->parse({{"path", path}}));
	return harness;
}

/// Member `member` of the record bank.
std::span<float> record(Harness& harness, std::size_t member) {
	return harness.input(harness.ports().bank(file::record.name).ports.at(member));
}

std::string contents(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// Whether the node made a handoff.
bool madeHandoff(const Harness& harness) { return !harness.handoffs().empty(); }

} // namespace

TEST_CASE("File declares inputs record.left and record.right and no outputs", "[file]") {
	TempFolder dir{"ports"};
	Recordings recordings(dir.path());
	CHECK(fileType().name() == "file");
	auto harness = parsed(recordings, "rec.wav");
	CHECK(harness->ports().inputs() == std::vector<ear::PortName>{"record.left", "record.right"});
	CHECK(harness->ports().outputs().empty());
}

TEST_CASE("File settings need exactly a non-empty string path", "[file]") {
	for (const ear::NodeSettings& settings :
	     {ear::NodeSettings{}, ear::NodeSettings::object(), ear::NodeSettings{{"path", ""}},
	      ear::NodeSettings{{"path", 7}}, ear::NodeSettings{{"path", "a.wav"}, {"gain", 1}},
	      ear::NodeSettings("a.wav")}) {
		INFO(settings.dump());
		Harness harness(fileType());
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

TEST_CASE("File fails its configure on a symlink in the folder that leads outside it", "[file]") {
	TempFolder dir{"symlink"};
	const auto outside = dir.path() / "outside.wav";
	std::ofstream(outside, std::ios::binary) << "keep";
	Recordings recordings(dir.path() / "recordings");
	std::filesystem::create_symlink(outside, dir.path() / "recordings" / "rec.wav");
	std::filesystem::create_directory_symlink(dir.path(), dir.path() / "recordings" / "up");

	for (const std::string path : {"rec.wav", "up/outside.wav", "up/new.wav"}) {
		INFO(path);
		auto harness = parsed(recordings, path);

		harness->add();

		CHECK(harness->phase() == "failed");
		REQUIRE(harness->health().reason);
		CHECK(harness->health().reason->code == ear::FailureReason::Code::startFailed);
		CHECK(harness->health().reason->detail == "cannot open " + path + ": it leads outside the folder");
		CHECK_FALSE(madeHandoff(*harness));
	}
	CHECK(contents(outside) == "keep");
	CHECK_FALSE(std::filesystem::exists(dir.path() / "new.wav"));
}

TEST_CASE("File records record.left and record.right as a 32-bit float stereo WAV at the format's rate", "[file]") {
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
	auto harness = parsed(recordings, "rec.wav", format);

	harness->add();
	harness->blockReady();
	CHECK(harness->phase() == "running");
	REQUIRE(harness->recordingRequests().size() == 1);
	CHECK(harness->recordingRequests()[0].path == "rec.wav");
	REQUIRE(harness->handoffs().size() == 1);
	CHECK(harness->handoffs()[0].bank == file::record.name);
	CHECK(harness->handoffs()[0].channels == 2);
	CHECK(harness->handoffs()[0].capacityFrames == 2 * format.rate); // 2 s ring
	CHECK(harness->runObject() != nullptr);                          // the open file

	std::vector<float> left;
	std::vector<float> right;
	for (std::uint32_t block = 0; block < blocks; ++block) {
		const std::span<float> fl = record(*harness, 0);
		const std::span<float> fr = record(*harness, 1);
		for (std::uint32_t i = 0; i < format.maxBlock; ++i) {
			const auto n = static_cast<float>(block * format.maxBlock + i);
			fl[i] = n / 1024.0F;
			fr[i] = -n / 2048.0F - 0.25F;
		}
		harness->process();
		left.insert(left.end(), fl.begin(), fl.end());
		right.insert(right.end(), fr.begin(), fr.end());
	}
	CHECK(harness->handoffFrames(0) == blocks * format.maxBlock);
	harness->remove(); // the handoff's consumer finishes the file
	CHECK(harness->gone());
	CHECK(harness->droppedFrames() == 0);

	SF_INFO info{};
	SNDFILE* file = sf_open(wav.c_str(), SFM_READ, &info);
	REQUIRE(file != nullptr);
	CHECK(info.samplerate == static_cast<int>(format.rate));
	CHECK(info.channels == 2);
	CHECK(info.format == (SF_FORMAT_WAV | SF_FORMAT_FLOAT));
	REQUIRE(info.frames == blocks * format.maxBlock);
	std::vector<float> read(static_cast<std::size_t>(info.frames) * 2);
	CHECK(sf_readf_float(file, read.data(), info.frames) == info.frames);
	sf_close(file);

	for (std::size_t frame = 0; frame < left.size(); ++frame) {
		INFO("frame " << frame);
		REQUIRE(read[2 * frame] == left[frame]);
		REQUIRE(read[2 * frame + 1] == right[frame]);
	}
}

TEST_CASE("File fails its configure on a path it cannot open, and asks for no handoff", "[file]") {
	TempFolder dir{"unopenable"};
	Recordings recordings(dir.path());
	auto harness = parsed(recordings, "missing-dir/rec.wav");

	harness->add();

	CHECK(harness->phase() == "failed");
	REQUIRE(harness->health().reason);
	CHECK(harness->health().reason->code == ear::FailureReason::Code::startFailed);
	CHECK(harness->health().reason->detail.starts_with("cannot open missing-dir/rec.wav: "));
	CHECK_FALSE(madeHandoff(*harness));
}

TEST_CASE("File fails its configure when the host has no recordings folder", "[file]") {
	Harness harness(fileType());
	REQUIRE_FALSE(harness.parse({{"path", "rec.wav"}}));
	harness.add();
	REQUIRE(harness.health().reason);
	CHECK(harness.health().reason->detail == "cannot open rec.wav: no recordings folder");
}
