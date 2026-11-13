// What the protocol library believes of the audio ear took from it, as ear's own reports leave it
// (docs/node-types.md#clock): sendspin-cpp's sync task counts what it wrote less what it was told played
// (`buffered_frames`, sync_task.cpp), and floors its playtime at now when that count is 0 (its underrun recovery, the
// carried sendspin-starved-floor.patch). Ear owes lost time only while the library cannot realign itself, so the two
// never both correct the same lost time. The data thread's own; nothing here locks, allocates or waits.
#pragma once
#include <cstdint>
namespace ear::sendspin {

class LibraryView {
public:
	/// Frames taken from the feed (skipped or read) in a cycle.
	void consumed(std::uint64_t frames) noexcept { consumed_ += frames; }
	/// The library was told everything taken so far.
	void told() noexcept { told_ = consumed_; }
	/// Taken and not yet told.
	std::uint64_t untold() const noexcept { return consumed_ - told_; }
	std::uint64_t consumedFrames() const noexcept { return consumed_; }
	/// The library's count of what it wrote and was not told played, with the feed holding `held`: what the feed holds
	/// and what ear took untold (what it is writing at the moment aside, which only adds).
	std::uint64_t buffered(std::uint32_t held) const noexcept { return held + untold(); }
	/// The library sees itself starved, and so realigns its next chunk to now on its own: it holds nothing it was not
	/// told played. Between two cycles the count only grows (the library only writes), so a check at a cycle's end
	/// holds until the next.
	bool starved(std::uint32_t held) const noexcept { return buffered(held) == 0; }

private:
	std::uint64_t consumed_ = 0;
	std::uint64_t told_ = 0;
};

} // namespace ear::sendspin
