// The "file" node type: inputs "FL" and "FR", recorded into a 32-bit float stereo WAV file at
// the graph frequency (docs/design/m1-white-to-file.md section 2, builtin.hpp). The realtime
// thread only hands frames to the handoff; the handoff's own thread writes them with libsndfile,
// and the file is complete (header finished) once the handoff has finished.
#include <sndfile.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ear/nodes/builtin.hpp"

namespace ear::nodes {
namespace {

constexpr std::string_view typeName = "file";
constexpr std::string_view pathKey = "path";
constexpr std::uint32_t channelCount = 2;
constexpr std::uint32_t ringSeconds = 2;

/// Owns the open file. The handoff thread writes and closes it; the destructor closes it only
/// if the handoff never finished (the framework destroys the insides after the handoff closed).
class WavWriter final : public HandoffConsumer {
public:
	explicit WavWriter(SNDFILE* file) noexcept : file_(file) {}
	WavWriter(const WavWriter&) = delete;
	WavWriter& operator=(const WavWriter&) = delete;
	~WavWriter() override { close(); }

	void consume(std::span<const float> interleaved, std::uint32_t frames) override {
		sf_writef_float(file_, interleaved.data(), frames);
	}

	void finish() override { close(); }

private:
	void close() noexcept {
		if (file_ != nullptr) {
			sf_close(file_);
			file_ = nullptr;
		}
	}

	SNDFILE* file_;
};

/// Passes FL then FR, in declaration order, to the handoff.
class Recorder final : public AudioCode {
public:
	void connect(RtWriter& writer) noexcept { writer_ = &writer; }

	void prepare(const AudioFormat&) override {}

	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>>,
	             std::uint32_t frames) noexcept override {
		// Frames the ring refuses are dropped and counted by the framework.
		writer_->write(inputs, frames);
	}

	void release() noexcept override {}

private:
	RtWriter* writer_ = nullptr;
};

class FileInsides final : public NodeInsides {
public:
	explicit FileInsides(SNDFILE* file) noexcept : wav_(file) {}

	AudioCode& audioCode() override { return recorder_; }
	HandoffConsumer& consumer() noexcept { return wav_; }
	void connect(RtWriter& writer) noexcept { recorder_.connect(writer); }

private:
	WavWriter wav_;
	Recorder recorder_;
};

class FileConfig final : public NodeConfig {
public:
	explicit FileConfig(std::string path) : path_(std::move(path)) {}

	const Ports& ports() const override { return ports_; }

	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& grant) const override {
		const AudioFormat format = grant.format();
		SF_INFO info{};
		info.samplerate = static_cast<int>(format.rate);
		info.channels = static_cast<int>(channelCount);
		info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
		// SFM_WRITE truncates an existing file.
		SNDFILE* file = sf_open(path_.c_str(), SFM_WRITE, &info);
		if (file == nullptr) {
			return std::unexpected(FailureReason{FailureReason::Code::startFailed,
			                                     "cannot open " + path_ + ": " + sf_strerror(nullptr)});
		}
		auto insides = std::make_unique<FileInsides>(file);
		insides->connect(grant.handoff(channelCount, ringSeconds * format.rate, insides->consumer()));
		return insides;
	}

private:
	Ports ports_{.inputs = {"FL", "FR"}, .outputs = {}};
	std::string path_;
};

class FileType final : public NodeType {
public:
	std::string_view name() const override { return typeName; }

	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings>
	parse(const NodeName& node, const NodeSettings& settings) const override {
		if (!settings.is_object()) {
			return std::unexpected(InvalidSettings{node, "settings: must be an object with \"path\""});
		}
		for (const auto& [key, value] : settings.items()) {
			if (key != pathKey) {
				return std::unexpected(InvalidSettings{node, "unknown key " + key});
			}
		}
		const auto path = settings.find(pathKey);
		if (path == settings.end()) {
			return std::unexpected(InvalidSettings{node, "path: missing"});
		}
		if (!path->is_string() || path->get_ref<const std::string&>().empty()) {
			return std::unexpected(InvalidSettings{node, "path: must be a non-empty string"});
		}
		return std::make_unique<const FileConfig>(path->get<std::string>());
	}
};

}  // namespace

std::unique_ptr<const NodeType> makeFileNodeType() { return std::make_unique<const FileType>(); }

}  // namespace ear::nodes
