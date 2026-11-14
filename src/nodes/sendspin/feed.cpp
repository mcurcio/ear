#include "feed.hpp"

#include <algorithm>
#include <utility>

#include <poll.h>
#include <time.h>

#include "ear/core/latency.hpp"

namespace ear::nodes::sendspin {
namespace {

constexpr std::uint64_t clearPending = std::uint64_t{1} << 32U;

} // namespace

std::int64_t rawNowNs() noexcept {
	timespec now{};
	::clock_gettime(CLOCK_MONOTONIC_RAW, &now);
	return std::int64_t{now.tv_sec} * 1'000'000'000 + now.tv_nsec;
}

Feed::Feed(std::uint32_t channels, std::uint32_t capacityFrames, std::optional<FeedDrain> drain)
	: ring_(channels, capacityFrames), drain_(drain) {}

bool Feed::applyClear() noexcept {
	const std::uint64_t asked = clearTo_.exchange(0, std::memory_order_acq_rel);
	if ((asked & clearPending) == 0) return false;
	ring_.skipTo(static_cast<std::uint32_t>(asked));
	clearedSince_ = true;
	return true;
}

/// The other half of write()'s check: this thread frees room, then looks; the writer says it waits, then looks. Each
/// fence puts one thread's look after the other's change, so a wake is never lost.
void Feed::wakeWriter() noexcept {
	std::atomic_thread_fence(std::memory_order_seq_cst);
	if (writerWaits_.exchange(false, std::memory_order_relaxed)) room_.ring();
}

/// One writer (the reading thread), so a plain store of the next value: no read-modify-write on the realtime path.
void Feed::countDry(std::uint32_t asked, std::uint32_t got) noexcept {
	if (got >= asked) return;
	dryReads_.store(dryReads_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	dryFrames_.store(dryFrames_.load(std::memory_order_relaxed) + (asked - got), std::memory_order_relaxed);
}

std::uint32_t Feed::read(std::span<const std::span<float>> channels, std::uint32_t frames) noexcept {
	const bool cleared = applyClear();
	const std::uint32_t got = ring_.readPlanar(channels, frames);
	for (const std::span<float> channel : channels) std::fill(channel.begin() + got, channel.begin() + frames, 0.0F);
	countDry(frames, got);
	if (cleared || got > 0) wakeWriter();
	return got;
}

std::uint32_t Feed::skip(std::uint64_t frames) noexcept {
	const bool cleared = applyClear();
	// What it holds now, on the reading thread, is exact: the writer only adds, so all of the smaller is skipped.
	heldAtSkip_ = ring_.writePosition() - ring_.readPosition();
	const std::uint32_t got = ring_.skip(static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, heldAtSkip_)));
	if (cleared || got > 0) wakeWriter();
	return got;
}

bool Feed::takeCleared() noexcept {
	if (applyClear()) wakeWriter();
	return std::exchange(clearedSince_, false);
}

std::uint32_t Feed::write(std::span<const float> interleaved, std::uint32_t frames,
                          std::chrono::milliseconds patience) {
	if (drain_) {
		const std::int64_t since = drain_->nowRawNs() - drainedRawNs_.load(std::memory_order_acquire);
		if (since > drain_->bound.count()) return discard(frames, patience);
		discarding_ = false;
	}
	const auto until = std::chrono::steady_clock::now() + patience;
	for (;;) {
		if (const std::uint32_t accepted = ring_.writeInterleaved(interleaved, frames); accepted > 0 || frames == 0) {
			return accepted;
		}
		writerWaits_.store(true, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (const std::uint32_t accepted = ring_.writeInterleaved(interleaved, frames); accepted > 0) {
			writerWaits_.store(false, std::memory_order_relaxed);
			return accepted;
		}
		const auto left = std::chrono::ceil<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
		if (left.count() <= 0) {
			writerWaits_.store(false, std::memory_order_relaxed);
			return 0;
		}
		pollfd ready{room_.fd(), POLLIN, 0};
		waitsBegun_.fetch_add(1, std::memory_order_release);
		if (::poll(&ready, 1, static_cast<int>(left.count())) > 0) waitsWoken_.fetch_add(1, std::memory_order_release);
		room_.drain();
		writerWaits_.store(false, std::memory_order_relaxed);
	}
}

void Feed::clear() noexcept {
	clearTo_.store(clearPending | ring_.writePosition(), std::memory_order_release);
	forgotten_.store(dropped_.load(std::memory_order_acquire),
	                 std::memory_order_release); // the library zeroes its count
}

/// The reader is gone (a stop, a stall past the bound): this side plays the frames into nothing at the graph's rate, a
/// feed's worth ahead of now at most, as a reader would have taken them, so the library's own thread runs at its own
/// pace and never pins its inbound ring behind audio no cycle will read. The library needs telling what played to run
/// at all (its stream's priming waits for the first report) and puts its next frame at the finish told plus what it
/// wrote and was not told, so each frame dropped is said, with when the next would play, for the notifier to tell.
std::uint32_t Feed::discard(std::uint32_t frames, std::chrono::milliseconds patience) {
	const std::int64_t now = drain_->nowRawNs();
	if (!discarding_) {
		discarding_ = true;
		paceRawNs_ = now;
		stale_.store(true, std::memory_order_release); // what the feed holds will not be read in its time
	}
	paceRawNs_ = std::max(paceRawNs_, now);
	const FrameClock clock(drain_->rate);
	const std::int64_t roomNs = clock.ns(ring_.capacityFrames()) - (paceRawNs_ - now);
	const std::int64_t room =
		std::int64_t{ring_.capacityFrames()} - clock.framesNearest(std::chrono::nanoseconds{paceRawNs_ - now});
	if (room <= 0) {
		// Until a frame's room comes, at most its patience.
		const auto untilRoom =
			std::chrono::ceil<std::chrono::milliseconds>(std::chrono::nanoseconds{clock.ns(1) - roomNs});
		waitForRoom(std::min(patience, untilRoom));
		return 0;
	}
	const auto accepted = static_cast<std::uint32_t>(std::min<std::int64_t>(frames, room));
	paceRawNs_ += clock.ns(accepted);
	finishRawNs_.store(paceRawNs_, std::memory_order_release);
	dropped_.fetch_add(accepted, std::memory_order_acq_rel);
	if (drain_->bell != nullptr) drain_->bell->ring();
	return accepted;
}

/// Waits up to `patience` for the clock to make room, or for a read (the reader back) to end the wait.
void Feed::waitForRoom(std::chrono::milliseconds patience) {
	writerWaits_.store(true, std::memory_order_relaxed);
	pollfd ready{room_.fd(), POLLIN, 0};
	::poll(&ready, 1, static_cast<int>(patience.count()));
	room_.drain();
	writerWaits_.store(false, std::memory_order_relaxed);
}

} // namespace ear::nodes::sendspin
