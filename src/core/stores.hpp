// The framework's policy for a node's store (docs/writing-a-node-type.md#stores): what its open reads, narrows, judges
// and creates, and what a write narrows and may replace, over a StoreAdapter that does only the disk's part. Private to
// ear_core: NodeHost runs these as jobs on the adapter's I/O executor, each holding the store's state by shared_ptr,
// never the configuration.
#pragma once
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ear/core/store.hpp"

namespace ear::detail {

/// One store of one configuration, shared by the configuration's Store and by every job of it, so a job that outlives
/// its configuration fills only this.
class StoreState {
public:
	/// `adapter` outlives every job.
	StoreState(StoreAdapter& adapter, std::string file, StoreRule rule)
		: adapter(adapter), file(std::move(file)), rule(rule) {}
	StoreAdapter& adapter;
	const std::string file;
	const StoreRule rule;

	/// Any thread: the text, once opened; std::logic_error (a type bug) before.
	std::string text() const;
	/// Any thread: its open completed and trusted it.
	bool opened() const;

	// The I/O thread's, one job at a time.
	/// The open: read (no symlink, at most maxBytes); a file there narrowed to the rule's mode before its text is
	/// judged (a file refused for its text may still hold a secret), then judged by the rule's distrust; a missing
	/// file created with the rule's initial text, or left missing without one. Never a new file over a distrusted one.
	/// The error is "<file>: <why>" (an adapter's errors name the file already). What the adapter throws passes on (its
	/// bug; `calling` names its call); a throw of the type's distrust or initial is the type's failure, "<file>:
	/// <distrust|initial> threw: <what>".
	std::expected<void, std::string> open();
	/// A write of `text`: narrowed first (so a mode widened since is never kept), then swapped in only if the file
	/// holds the text last read or written. The error names the file; the adapter's throws pass on.
	std::expected<void, std::string> write(std::string text);
	/// The adapter call under way, for a throw's record ("StoreAdapter::read").
	std::string_view calling = "StoreAdapter";

private:
	mutable std::mutex mutex_;
	bool opened_ = false;             // mutex_
	std::optional<std::string> text_; // mutex_: what the file held as last read or written; nothing: no file
};

} // namespace ear::detail
