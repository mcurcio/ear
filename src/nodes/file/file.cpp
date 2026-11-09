// The "file" node type: inputs "record.left" and "record.right", recorded into a 32-bit float stereo WAV file at the
// graph frequency (ear/nodes/file.hpp; docs/node-types.md#file). Its configure opens the recording (beneath the
// recordings folder, by the framework) and asks for a sink on its inside output bank `writer`; the framework's
// pass-through copies `record` to `writer` on the realtime thread, the block's tap hands each cycle to the sink, and
// the sink's own thread runs WavWriter: it opens the WAV in begin (the header written there, never on the main loop),
// writes each chunk, and finishes the file once the node's configuration is torn down.
#include <sndfile.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "ear/nodes/file.hpp"
#include "file_spec.hpp"

namespace ear::nodes::file {
namespace {

/// The sink's consumer: opens the WAV on the sink's thread, writes each chunk, finishes the header. Owns the
/// open file, not its descriptor (the framework's recording, closed after the sink: teardown step 5).
class WavWriter final : public SinkConsumer {
public:
	WavWriter(int descriptor, std::string path, AudioFormat format, InsidesEvents& events)
		: descriptor_(descriptor), path_(std::move(path)), format_(format), events_(events) {}
	WavWriter(const WavWriter&) = delete;
	WavWriter& operator=(const WavWriter&) = delete;
	~WavWriter() override { close(); }

	void begin() override {
		SF_INFO info{};
		info.samplerate = static_cast<int>(format_.rate);
		info.channels = 2;
		info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
		std::string refused;
		{
			// sf_strerror(nullptr) reads libsndfile's one process-wide error, and every file node's sink opens on a
			// thread of its own: the open and its error are read together, under one lock.
			static std::mutex opening;
			const std::scoped_lock lock(opening);
			file_ = sf_open_fd(descriptor_, SFM_WRITE, &info, SF_FALSE);
			if (file_ == nullptr) refused = sf_strerror(nullptr);
		}
		// Said before the sink posts `begun`, so the node fails start-failed before it is configured.
		if (file_ == nullptr) events_.lost("cannot open " + path_ + ": " + refused);
	}

	void consume(const Chunk& chunk) override {
		if (file_ == nullptr) return;
		const sf_count_t written = sf_writef_float(file_, chunk.interleaved.data(), chunk.frames);
		// A short write (a full disk) loses frames from the WAV: said once, as a note (3a gives a type no log seam).
		if (written < static_cast<sf_count_t>(chunk.frames) && !shortSaid_) {
			shortSaid_ = true;
			events_.say("write-short " + path_ + ": " + sf_strerror(file_));
		}
	}

	void finish() override { close(); }

private:
	void close() noexcept {
		if (file_ != nullptr) sf_close(file_);
		file_ = nullptr;
	}

	int descriptor_;
	std::string path_;
	AudioFormat format_;
	InsidesEvents& events_; // the configuration's channel, which outlives the sink
	SNDFILE* file_ = nullptr;
	bool shortSaid_ = false; // the sink's thread only
};

} // namespace

Configured File::configure(CreateOf<spec>& cx, const Checked& s) {
	const std::string path = s.get<pathRow>();
	// The framework creates or truncates the file beneath the recordings folder; libsndfile only writes to it.
	const auto descriptor = cx.openRecording(RecordingRequest{path}); // 3a interim: synchronous
	if (!descriptor) return Configured::failed("cannot open " + path + ": " + descriptor.error());
	cx.sink(writer, SinkShape{.frames = ringSeconds * cx.format().rate})
		.consumedBy(std::make_unique<WavWriter>(*descriptor, path, cx.format(), node_.handle()));
	cx.use(passThrough({bankPair(record, writer)}));
	return Configured::applied();
}

} // namespace ear::nodes::file

namespace ear::nodes {

std::unique_ptr<const NodeType> makeFileNodeType() { return makeNodeType<file::spec, file::File>(); }

} // namespace ear::nodes
