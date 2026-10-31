// The "file" node type through its NodeType contract, with a synchronous fake grant standing
// in for the framework's handoff (docs/design/m1-white-to-file.md section 3, slice S5 criteria
// 2 and 3). The WAV files are read back with libsndfile. Paths are relative to a recordings folder
// and never lead out of it (docs/design/batch2.md section 4.4).
#include <sndfile.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/nodes/builtin.hpp"

namespace {

/// A handoff without a ring or a thread: write() interleaves its channels and gives them to the
/// consumer at once, as the framework's handoff thread would later.
class SynchronousWriter final : public ear::RtWriter {
public:
	SynchronousWriter(std::uint32_t channels, ear::HandoffConsumer& consumer)
	    : channels_(channels), consumer_(consumer) {}

	std::uint32_t write(std::span<const std::span<const float>> channels, std::uint32_t frames) noexcept override {
		std::vector<float> interleaved;
		interleaved.reserve(static_cast<std::size_t>(frames) * channels_);
		for (std::uint32_t frame = 0; frame < frames; ++frame) {
			for (std::uint32_t channel = 0; channel < channels_; ++channel) {
				interleaved.push_back(channels[channel][frame]);
			}
		}
		consumer_.consume(interleaved, frames);
		return frames;
	}

	ear::HandoffConsumer& consumer() const noexcept { return consumer_; }

private:
	std::uint32_t channels_;
	ear::HandoffConsumer& consumer_;
};

class FakeGrant final : public ear::NodeGrant {
public:
	explicit FakeGrant(ear::AudioFormat format) : format_(format) {}
	ear::AudioFormat format() const override { return format_; }
	ear::RtWriter& handoff(std::uint32_t channels, std::uint32_t capacityFrames,
	                       ear::HandoffConsumer& consumer) override {
		requests.push_back({channels, capacityFrames});
		writers.push_back(std::make_unique<SynchronousWriter>(channels, consumer));
		return *writers.back();
	}
	void soundCard(const ear::SoundCardRequest&) override {
		throw std::logic_error("file asked for a sound card");
	}
	void rtpReceiver(const ear::RtpReceiverRequest&) override {
		throw std::logic_error("file asked for an RTP receiver");
	}

	struct Request {
		std::uint32_t channels;
		std::uint32_t capacityFrames;
	};
	std::vector<Request> requests;
	std::vector<std::unique_ptr<SynchronousWriter>> writers;

private:
	ear::AudioFormat format_;
};

/// A path in a fresh directory of its own, removed with it.
class TempDir {
public:
	explicit TempDir(const std::string& name)
	    : path_(std::filesystem::temp_directory_path() / ("ear_nodes_" + name + "_" + std::to_string(getpid()))) {
		std::filesystem::remove_all(path_);
		std::filesystem::create_directories(path_);
	}
	~TempDir() { std::filesystem::remove_all(path_); }
	TempDir(const TempDir&) = delete;
	TempDir& operator=(const TempDir&) = delete;
	const std::filesystem::path& path() const noexcept { return path_; }

private:
	std::filesystem::path path_;
};

std::shared_ptr<const ear::nodes::RecordingsFolder> recordingsIn(const std::filesystem::path& path) {
	auto folder = ear::nodes::RecordingsFolder::open(path);
	REQUIRE(folder.has_value());
	return std::make_shared<const ear::nodes::RecordingsFolder>(std::move(*folder));
}

std::unique_ptr<const ear::NodeConfig> parseFile(const std::shared_ptr<const ear::nodes::RecordingsFolder>& folder,
                                                 const std::string& path) {
	auto config = ear::nodes::makeFileNodeType(folder)->parse("rec", {{"path", path}});
	REQUIRE(config.has_value());
	return std::move(*config);
}

std::string contents(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("File declares inputs FL and FR and no outputs", "[file]") {
	TempDir dir{"ports"};
	const auto type = ear::nodes::makeFileNodeType(recordingsIn(dir.path()));
	CHECK(type->name() == "file");
	const auto config = parseFile(recordingsIn(dir.path()), "rec.wav");
	CHECK(config->ports().inputs == std::vector<ear::PortName>{"FL", "FR"});
	CHECK(config->ports().outputs.empty());
}

TEST_CASE("File settings need exactly a non-empty string path", "[file]") {
	TempDir dir{"settings"};
	const auto type = ear::nodes::makeFileNodeType(recordingsIn(dir.path()));
	for (const ear::NodeSettings& settings :
	     {ear::NodeSettings{}, ear::NodeSettings::object(), ear::NodeSettings{{"path", ""}},
	      ear::NodeSettings{{"path", 7}}, ear::NodeSettings{{"path", "a.wav"}, {"gain", 1}},
	      ear::NodeSettings("a.wav")}) {
		INFO(settings.dump());
		const auto config = type->parse("rec", settings);
		REQUIRE_FALSE(config.has_value());
		CHECK(config.error().node == "rec");
		CHECK_FALSE(config.error().detail.empty());
	}
}

TEST_CASE("File refuses an absolute path or a .. component as invalid settings", "[file]") {
	TempDir dir{"outside"};
	const auto type = ear::nodes::makeFileNodeType(recordingsIn(dir.path() / "recordings"));
	for (const std::string& path : {std::string("/tmp/rec.wav"), (dir.path() / "rec.wav").string(), std::string("../x.wav"),
	                               std::string(".."), std::string("take/../../x.wav"), std::string("take/..")}) {
		INFO(path);
		const auto config = type->parse("rec", {{"path", path}});
		REQUIRE_FALSE(config.has_value());
		CHECK(config.error().node == "rec");
		CHECK(config.error().detail.find("recordings folder") != std::string::npos);
	}
	for (const std::string path : {"rec.wav", "take/rec.wav", "..rec.wav", "./rec.wav"}) {
		INFO(path);
		CHECK(type->parse("rec", {{"path", path}}).has_value());
	}
}

TEST_CASE("File refuses at start a symlink in the folder that leads outside it", "[file]") {
	TempDir dir{"symlink"};
	const auto outside = dir.path() / "outside.wav";
	std::ofstream(outside, std::ios::binary) << "keep";
	const auto folder = recordingsIn(dir.path() / "recordings");
	std::filesystem::create_symlink(outside, dir.path() / "recordings" / "rec.wav");
	std::filesystem::create_directory_symlink(dir.path(), dir.path() / "recordings" / "up");

	for (const std::string path : {"rec.wav", "up/outside.wav", "up/new.wav"}) {
		INFO(path);
		const auto config = parseFile(folder, path);
		FakeGrant grant{{.rate = 48000, .maxBlock = 256}};

		const auto insides = config->start(grant);

		REQUIRE_FALSE(insides.has_value());
		CHECK(insides.error().code == ear::FailureReason::Code::startFailed);
		CHECK(insides.error().detail == "cannot open " + path + ": it leads outside the recordings folder");
		CHECK(grant.requests.empty());
	}
	CHECK(contents(outside) == "keep");
	CHECK_FALSE(std::filesystem::exists(dir.path() / "new.wav"));
}

TEST_CASE("File records FL and FR as a 32-bit float stereo WAV at the format's rate", "[file]") {
	TempDir dir{"records"};
	const auto wav = dir.path() / "rec.wav";
	{
		// Longer than what the node writes: the node must truncate it.
		std::ofstream old{wav, std::ios::binary};
		old << std::string(1 << 20, 'x');
	}
	constexpr ear::AudioFormat format{.rate = 32000, .maxBlock = 256};
	constexpr std::uint32_t blocks = 3;
	const auto config = parseFile(recordingsIn(dir.path()), "rec.wav");
	FakeGrant grant{format};

	auto insides = (*config).start(grant);
	REQUIRE(insides.has_value());
	REQUIRE(grant.requests.size() == 1);
	CHECK(grant.requests[0].channels == 2);
	CHECK(grant.requests[0].capacityFrames == 2 * format.rate);   // 2 s ring

	ear::AudioCode& code = (*insides)->audioCode();
	code.prepare(format);
	std::vector<float> left;
	std::vector<float> right;
	for (std::uint32_t block = 0; block < blocks; ++block) {
		std::vector<float> fl(format.maxBlock);
		std::vector<float> fr(format.maxBlock);
		for (std::uint32_t i = 0; i < format.maxBlock; ++i) {
			const auto n = static_cast<float>(block * format.maxBlock + i);
			fl[i] = n / 1024.0F;
			fr[i] = -n / 2048.0F - 0.25F;
		}
		const std::array<std::span<const float>, 2> inputs{fl, fr};
		code.process(inputs, {}, format.maxBlock);
		left.insert(left.end(), fl.begin(), fl.end());
		right.insert(right.end(), fr.begin(), fr.end());
	}
	code.release();
	grant.writers[0]->consumer().finish();
	insides->reset();

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

TEST_CASE("File fails to start on a path it cannot open, and asks for no handoff", "[file]") {
	TempDir dir{"unopenable"};
	const auto config = parseFile(recordingsIn(dir.path()), "missing-dir/rec.wav");
	FakeGrant grant{{.rate = 48000, .maxBlock = 256}};

	const auto insides = config->start(grant);

	REQUIRE_FALSE(insides.has_value());
	CHECK(insides.error().code == ear::FailureReason::Code::startFailed);
	CHECK(insides.error().detail.find("missing-dir") != std::string::npos);
	CHECK(grant.requests.empty());
}
