// The "file" node type: inputs "record.left" and "record.right", recorded into a 32-bit float stereo WAV file at
// the graph frequency (docs/design/m1-white-to-file.md section 2, ear/nodes/file.hpp). The realtime
// thread only hands frames to the handoff; the handoff's own thread writes them with libsndfile,
// and the file is complete (header finished) once the handoff has finished. The path is relative
// to the recordings folder and opened beneath it only (ear/folder/confined_folder.hpp).
#include <sndfile.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <unistd.h>

#include "ear/core/settings_table.hpp"
#include "ear/folder/confined_folder.hpp"
#include "ear/nodes/file.hpp"

namespace ear::nodes {
namespace {

/// The path as given, nothing when empty.
std::optional<std::string> nonEmpty(std::string_view value) {
	if (value.empty()) return std::nullopt;
	return std::string(value);
}

constexpr auto pathRow = setting::required("path", setting::text<nonEmpty>("a non-empty string"));
constexpr SettingsTable<pathRow> table{"file"};
constexpr std::uint32_t channelCount = 2;
constexpr std::uint32_t ringSeconds = 2;

/// Owns a descriptor from folder::ConfinedFolder::create; libsndfile is told not to close it.
class Descriptor {
public:
	explicit Descriptor(int fd) noexcept : fd_(fd) {}
	Descriptor(Descriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
	Descriptor& operator=(Descriptor&&) = delete;
	Descriptor(const Descriptor&) = delete;
	Descriptor& operator=(const Descriptor&) = delete;
	~Descriptor() { close(); }
	int get() const noexcept { return fd_; }
	void close() noexcept {
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
	}

private:
	int fd_;
};

/// Owns the open file. The handoff thread writes and closes it; the destructor closes it only
/// if the handoff never finished (the framework destroys the insides after the handoff closed).
class WavWriter final : public HandoffConsumer {
public:
	WavWriter(SNDFILE* file, Descriptor fd) noexcept : file_(file), fd_(std::move(fd)) {}
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
		fd_.close();
	}

	SNDFILE* file_;
	Descriptor fd_;
};

/// Passes left then right, in declaration order, to the handoff.
class Recorder final : public AudioCode {
public:
	void connect(RtWriter& writer) noexcept { writer_ = &writer; }

	void prepare(const AudioFormat&) override {}

	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>>,
	             std::uint32_t frames, const GraphCycle&) noexcept override {
		// Frames the ring refuses are dropped and counted by the framework.
		writer_->write(inputs, frames);
	}

	void release() noexcept override {}

private:
	RtWriter* writer_ = nullptr;
};

class FileInsides final : public NodeInsides {
public:
	FileInsides(SNDFILE* file, Descriptor fd) noexcept : wav_(file, std::move(fd)) {}

	AudioCode& audioCode() override { return recorder_; }
	HandoffConsumer& consumer() noexcept { return wav_; }
	void connect(RtWriter& writer) noexcept { recorder_.connect(writer); }

private:
	WavWriter wav_;
	Recorder recorder_;
};

class FileConfig final : public NodeConfig {
public:
	FileConfig(std::shared_ptr<const folder::ConfinedFolder> recordings, std::string path)
		: recordings_(std::move(recordings)), path_(std::move(path)) {}

	const Ports& ports() const override { return ports_; }

	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& grant) const override {
		const AudioFormat format = grant.format();
		// The folder creates or truncates the file; libsndfile only writes to the descriptor.
		auto opened = recordings_->create(path_);
		if (!opened) {
			return std::unexpected(
				FailureReason{FailureReason::Code::startFailed, "cannot open " + path_ + ": " + opened.error().detail});
		}
		Descriptor fd(*opened);
		SF_INFO info{};
		info.samplerate = static_cast<int>(format.rate);
		info.channels = static_cast<int>(channelCount);
		info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
		SNDFILE* file = sf_open_fd(fd.get(), SFM_WRITE, &info, SF_FALSE);
		if (file == nullptr) {
			return std::unexpected(
				FailureReason{FailureReason::Code::startFailed, "cannot open " + path_ + ": " + sf_strerror(nullptr)});
		}
		auto insides = std::make_unique<FileInsides>(file, std::move(fd));
		insides->connect(grant.handoff(channelCount, ringSeconds * format.rate, insides->consumer()));
		return insides;
	}

private:
	Ports ports_{{{Direction::input, "record", {"left", "right"}}}};
	std::shared_ptr<const folder::ConfinedFolder> recordings_;
	std::string path_;
};

/// The settings rule: a path relative to the recordings folder that names no "..". The folder's
/// open enforces the same at every lookup step (symlinks included); this check makes the
/// mistake an invalid-settings error, before any node starts.
bool staysInside(const std::string& path) {
	const std::filesystem::path relative(path);
	if (relative.is_absolute()) return false;
	for (const auto& component : relative) {
		if (component == "..") return false;
	}
	return true;
}

class FileType final : public NodeTypeOf<table> {
public:
	explicit FileType(std::shared_ptr<const folder::ConfinedFolder> recordings) : recordings_(std::move(recordings)) {}

private:
	MadeConfig make(const NodeName&, const Checked& settings) const override {
		const std::string& path = settings.get<pathRow>();
		if (!staysInside(path)) {
			return std::unexpected(std::string(R"(path: must be relative to the recordings folder, without "..")"));
		}
		return std::make_unique<const FileConfig>(recordings_, path);
	}

	std::shared_ptr<const folder::ConfinedFolder> recordings_;
};

} // namespace

std::unique_ptr<const NodeType> makeFileNodeType(const NodeFolders& folders) {
	return std::make_unique<const FileType>(folders.recordings);
}

} // namespace ear::nodes
