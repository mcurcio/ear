// The "file" node type: inputs "record.left" and "record.right", recorded into a 32-bit float stereo WAV file at
// the graph frequency (docs/design/m1-white-to-file.md section 2, ear/nodes/file.hpp; docs/design/node-framework.md
// sections 7.1 to 7.3). Its start acquires the recording (opened beneath the recordings folder by the framework) and a
// handoff; the realtime thread only hands frames to the handoff, whose own thread writes them with libsndfile, and the
// file is complete (header finished) once the handoff has finished.
#include <sndfile.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ear/core/type_spec.hpp"
#include "ear/nodes/file.hpp"

namespace ear::nodes {
namespace {

/// The path as given, nothing when empty.
std::optional<std::string> nonEmpty(std::string_view value) {
	if (value.empty()) return std::nullopt;
	return std::string(value);
}

constexpr auto pathRow = setting::required("path", setting::text<nonEmpty>("a non-empty string"));
constexpr auto record = bank::input<"record">;
constexpr TypeSpec spec{.settings = SettingsTable<pathRow>{"file"}, .banks = Banks<record>{}};
using Checked = CheckedOf<spec>;
constexpr std::uint32_t channelCount = 2;
constexpr std::uint32_t ringSeconds = 2;

/// Owns the open file (not its descriptor, the framework's recording). The handoff thread writes and finishes it; the
/// destructor finishes it only if the handoff never did.
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

/// Passes left then right, in declaration order, to the handoff. Frames the ring refuses are dropped and counted by the
/// framework.
struct Recorder {
	RtWriter* writer;
	void process(Io<spec> io, const GraphCycle&) noexcept { writer->write(io[record], io.frames()); }
};

/// The run: the open file, the handoff's consumer. It outlives the handoff (the framework closes the handoff first).
class FileRun final : public RunObject {
public:
	explicit FileRun(SNDFILE* file) noexcept : wav_(file) {}
	HandoffConsumer& consumer() noexcept { return wav_; }

private:
	WavWriter wav_;
};

struct File {
	static Members<spec> ports(const Checked&) { return {record(members("left", "right"))}; }

	/// A path relative to the recordings folder that names no "..". The recording's open enforces the same at every
	/// lookup step (symlinks included); this check makes the mistake an invalid-settings error, before any node starts.
	static std::expected<void, std::string> check(const Checked& s) {
		const std::filesystem::path relative(s.get<pathRow>());
		bool inside = !relative.is_absolute();
		for (const auto& component : relative) inside = inside && component != "..";
		if (!inside)
			return std::unexpected(std::string(R"(path: must be relative to the recordings folder, without "..")"));
		return {};
	}

	static StartResult start(StartContext& context, const Checked& s) {
		const std::string path = s.get<pathRow>();
		const AudioFormat format = context.format();
		// The framework creates or truncates the file beneath the recordings folder; libsndfile only writes to it.
		const auto opened = context.acquire(RecordingRequest{path});
		if (!opened)
			return std::unexpected(
				FailureReason{FailureReason::Code::startFailed, "cannot open " + path + ": " + opened.error()});
		SF_INFO info{};
		info.samplerate = static_cast<int>(format.rate);
		info.channels = static_cast<int>(channelCount);
		info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
		SNDFILE* file = sf_open_fd(*opened, SFM_WRITE, &info, SF_FALSE);
		if (file == nullptr)
			return std::unexpected(
				FailureReason{FailureReason::Code::startFailed, "cannot open " + path + ": " + sf_strerror(nullptr)});
		auto run = std::make_unique<FileRun>(file);
		RtWriter& writer =
			context.acquire(HandoffRequest{std::string(record.name), ringSeconds * format.rate, &run->consumer()});
		return {Recorder{&writer},
		        [run = std::move(run)](Ready&) mutable -> std::expected<std::unique_ptr<RunObject>, FailureReason> {
					return std::move(run);
				}};
	}
};

} // namespace

std::unique_ptr<const NodeType> makeFileNodeType() { return makeNodeType<spec, File>(); }

} // namespace ear::nodes
