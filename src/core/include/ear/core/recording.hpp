// A node's recordings (docs/writing-a-node-type.md#recordings): files beneath the recordings folder that one
// configuration writes, asked for in configure (CreateOf::recording), claimed at the first readiness step (the claim
// "recording:<path>", so two nodes never write one file), created or truncated on the I/O thread at step `opens`, and
// closed there once the configuration is freed (teardown step 5). What a type writes against (RecordingRequest,
// Recording) and the disk the framework opens them through (RecordingAdapter: ear_runtime's ConfinedRecordings over
// the recordings folder and the I/O thread; the harness's in memory). Pure: nothing but the standard library and
// ear_core.
#pragma once
#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include "ear/core/graph_spec.hpp"
#include "ear/core/io_executor.hpp"
namespace ear {

/// A recording file: `path`, relative to the recordings folder, created or truncated beneath it only (no "..", no
/// symlink out, no subfolder made).
struct RecordingRequest {
	std::string path;
};

/// A recording's open is a readiness item with this bound (docs/engine.md#readiness-deadlines).
inline constexpr std::chrono::milliseconds recordingBound{5'000};

/// A recording's claim value: `path` with its "." and empty components removed ("./take//1.wav" is "take/1.wav"), so
/// two spellings of one path are one claim. Symlinks are not followed: two paths that reach one file through a symlink
/// are two claims.
std::string recordingClaimPath(std::string_view path);

/// A recording as a type sees it (cx.recording(request)): valid from configure until the configuration is freed
/// (teardown step 5), open once the node is configured, so from its sink consumer's begin() on.
class Recording {
public:
	/// Any thread, once opened: the file's descriptor, owned by the framework (never close it; libsndfile's
	/// sf_open_fd takes it with SF_FALSE). Before its open landed, a std::logic_error (a type bug: the node fails).
	virtual int descriptor() const = 0;
	/// The path as the type asked for it.
	virtual const std::string& path() const noexcept = 0;

protected:
	~Recording() = default;
};

/// One open recording file, as the adapter made it. Destroying it closes it; the framework destroys it on the I/O
/// thread.
class RecordingFile {
public:
	virtual ~RecordingFile() = default;
	virtual int descriptor() const noexcept = 0;
};

/// The disk under the nodes' recordings (ResourceAdapters::recordings): ear_runtime's ConfinedRecordings, over the
/// recordings folder held open from boot and the I/O thread. The disk's part only: NodeHost decides when a recording
/// is claimed, opened and closed. A std::logic_error from open, or an open that returns no file, is its bug
/// (docs/architecture.md#faults-by-owner): the node fails lost, ear.internal-error; anything else it throws is a
/// refusal, as an error is.
class RecordingAdapter {
public:
	/// Where every recording is opened and closed, one job at a time in post order; outlives every recording.
	virtual IoExecutor& io() noexcept = 0;
	/// io()'s thread, blocking: `path` created or truncated beneath the folder only. The error is why not (it names no
	/// path).
	virtual std::expected<std::unique_ptr<RecordingFile>, std::string> open(const NodeName& node,
	                                                                        const std::string& path) = 0;

protected:
	~RecordingAdapter() = default;
};

} // namespace ear
