#include "feed.hpp"

#include <algorithm>
#include <thread>
#include <utility>

#include <poll.h>
#include <time.h>

#include "ear/core/latency.hpp"

namespace ear::nodes::sendspin {
namespace {

constexpr std::uint64_t clearPending = std::uint64_t{1} << 32U;
constexpr std::uint64_t reclaimedBit = std::uint64_t{1} << 63U;

} // namespace

std::int64_t rawNowNs() noexcept {
	timespec now{};
	::clock_gettime(CLOCK_MONOTONIC_RAW, &now);
	return std::int64_t{now.tv_sec} * 1'000'000'000 + now.tv_nsec;
}

Feed::Feed(std::uint32_t channels, std::uint32_t capacityFrames, std::optional<FeedDrain> drain)
	: ring_(channels, capacityFrames), drain_(drain), drainedRawNs_(drain ? drain->nowRawNs() : 0) {}

/// The reader's moves are bracketed by a sequence count it alone writes, odd while it moves, so readerMark() reads the
/// read position, the reader's own count and its clears of one instant. Only the writer waits on it, never the reader.
void Feed::beginMove() noexcept {
	readSeq_.store(readSeq_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
}

void Feed::endMove() noexcept {
	readSeq_.store(readSeq_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

/// Inside a move.
bool Feed::applyClear() noexcept {
	const std::uint64_t asked = clearTo_.exchange(0, std::memory_order_acq_rel);
	if ((asked & clearPending) == 0) return false;
	ring_.skipTo(static_cast<std::uint32_t>(asked));
	consumedAtClear_ = consumed_.load(std::memory_order_relaxed);
	clears_.store(clears_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	clearedSince_ = true;
	return true;
}

Feed::ReaderMark Feed::readerMark() const noexcept {
	for (;;) {
		const std::uint32_t before = readSeq_.load(std::memory_order_acquire);
		if ((before & 1U) == 0) {
			const std::uint64_t asked = clearTo_.load(std::memory_order_relaxed);
			const std::uint32_t read = ring_.readPosition();
			const std::uint32_t consumed = consumed_.load(std::memory_order_relaxed);
			const std::uint32_t clears = clears_.load(std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_acquire);
			if (readSeq_.load(std::memory_order_relaxed) == before) {
				// A clear asked and not yet applied discards what lies before its position: none of that will play, and
				// the library forgot it, so none of it is counted stale.
				const auto clearAt = static_cast<std::uint32_t>(asked);
				const bool pending = (asked & clearPending) != 0 && static_cast<std::int32_t>(clearAt - read) > 0;
				return {.from = pending ? clearAt : read, .consumed = consumed, .clears = clears};
			}
		}
		std::this_thread::yield(); // the reader is mid-move, for the microseconds of one read
	}
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
	beginMove();
	const bool cleared = applyClear();
	const std::uint32_t got = ring_.readPlanar(channels, frames);
	consumed_.store(consumed_.load(std::memory_order_relaxed) + got, std::memory_order_relaxed);
	endMove();
	for (const std::span<float> channel : channels) std::fill(channel.begin() + got, channel.begin() + frames, 0.0F);
	countDry(frames, got);
	if (cleared || got > 0) wakeWriter();
	return got;
}

std::uint32_t Feed::skip(std::uint64_t frames) noexcept {
	beginMove();
	const bool cleared = applyClear();
	// What it holds now, on the reading thread, is exact: the writer only adds, so all of the smaller is skipped.
	heldAtSkip_ = ring_.writePosition() - ring_.readPosition();
	const std::uint32_t got = ring_.skip(static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, heldAtSkip_)));
	consumed_.store(consumed_.load(std::memory_order_relaxed) + got, std::memory_order_relaxed);
	endMove();
	if (cleared || got > 0) wakeWriter();
	return got;
}

std::optional<Feed::StaleSkip> Feed::skipStale() noexcept {
	if (!stale_.exchange(false, std::memory_order_acq_rel)) return std::nullopt;
	// Taken back: every drop counted from here is refused, so the last finish published is the drop's last.
	dropped_.fetch_or(reclaimedBit, std::memory_order_acq_rel);
	const std::uint32_t from = staleFrom_.load(std::memory_order_relaxed);
	const std::uint32_t to = staleTo_.load(std::memory_order_relaxed);
	const std::uint32_t markConsumed = staleConsumed_.load(std::memory_order_relaxed);
	const std::uint32_t markClears = staleClears_.load(std::memory_order_relaxed);
	beginMove();
	const std::uint32_t read = ring_.readPosition();
	ring_.skipTo(to);
	endMove();
	// Read after the writer counted it: what the reader read of its own since the mark, or since the last clear it
	// applied after the mark (what it read before that clear was of a stream the library forgot). Never from the read
	// position, which a clear moves too.
	const std::uint32_t since =
		consumed_.load(std::memory_order_relaxed) -
		(clears_.load(std::memory_order_relaxed) != markClears ? consumedAtClear_ : markConsumed);
	const std::uint32_t readSince = std::min(since, to - from);
	wakeWriter();
	return StaleSkip{.skipped = static_cast<std::int32_t>(to - read) > 0 ? to - read : 0, .readSince = readSince};
}

bool Feed::takeCleared() noexcept {
	beginMove();
	const bool cleared = applyClear();
	endMove();
	if (cleared) wakeWriter();
	return std::exchange(clearedSince_, false);
}

std::optional<Feed::Dropped> Feed::tryDropped() const noexcept {
	const std::uint64_t before = dropSeq_.load(std::memory_order_acquire);
	if ((before & 1U) != 0) return std::nullopt;
	const std::uint64_t total = dropped_.load(std::memory_order_relaxed) & ~reclaimedBit;
	const std::int64_t finish = finishRawNs_.load(std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_acquire);
	if (dropSeq_.load(std::memory_order_relaxed) != before) return std::nullopt;
	return Dropped{.total = total, .forgotten = forgotten_.load(std::memory_order_acquire), .finishRawNs = finish};
}

Feed::Dropped Feed::dropped() const noexcept {
	for (;;) {
		if (const auto dropped = tryDropped()) return *dropped;
		std::this_thread::yield(); // the writer is mid-publish, for a few stores
	}
}

std::uint32_t Feed::write(std::span<const float> interleaved, std::uint32_t frames,
                          std::chrono::milliseconds patience) {
	if (drain_) {
		const std::int64_t since = drain_->nowRawNs() - drainedRawNs_.load(std::memory_order_acquire);
		if (since > drain_->bound.count()) {
			const std::uint32_t accepted = discard(frames, patience);
			if (discarding_) return accepted;
			// Taken back as it dropped: the reader is back, so these frames are the ring's.
		}
		discarding_ = false;
	}
	return writeRing(interleaved, frames, patience);
}

std::uint32_t Feed::writeRing(std::span<const float> interleaved, std::uint32_t frames,
                              std::chrono::milliseconds patience) {
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
	forgotten_.store(dropped_.load(std::memory_order_acquire) & ~reclaimedBit,
	                 std::memory_order_release); // the library zeroes its count
}

/// One drop counted, under the writer's sequence count, so dropped() never pairs a total with another drop's finish.
/// Refused once the reader took the feed back, unless it `begins` a drop (which clears that mark: the reader cannot
/// take back a drop before it is marked stale, which follows this).
bool Feed::publishDrop(std::uint64_t frames, std::int64_t finishRawNs, bool begins) noexcept {
	const std::uint64_t sequence = dropSeq_.load(std::memory_order_relaxed);
	dropSeq_.store(sequence + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	std::uint64_t total = dropped_.load(std::memory_order_relaxed);
	bool counted = false;
	while (begins || (total & reclaimedBit) == 0) {
		if (dropped_.compare_exchange_weak(total, (total & ~reclaimedBit) + frames, std::memory_order_acq_rel,
		                                   std::memory_order_relaxed)) {
			counted = true;
			break;
		}
	}
	if (counted) finishRawNs_.store(finishRawNs, std::memory_order_relaxed);
	dropSeq_.store(sequence + 2, std::memory_order_release);
	if (counted && drain_->bell != nullptr) drain_->bell->ring();
	return counted;
}

/// The reader is gone (a stop, a stall past the bound): this side plays the frames into nothing at the graph's rate, up
/// to now and never ahead of it, as a reader would have taken them, so the library's own thread runs at its own pace
/// and never pins its inbound ring behind audio no cycle will read. Never ahead, so every finish said is in the past
/// (plus what lies downstream) when a cycle comes back, and the code's next finish, dated by that cycle, is no
/// earlier: what the library is told never steps back. The library needs telling what played to run at all (its
/// stream's priming waits for the first report) and puts its next frame at the finish told plus what it wrote and was
/// not told, so each frame dropped is said, with when the next would be heard, for the notifier to tell. Ends
/// (discarding_ false) when the reader took the feed back.
std::uint32_t Feed::discard(std::uint32_t frames, std::chrono::milliseconds patience) {
	const std::int64_t now = drain_->nowRawNs();
	const std::int64_t downstream = downstreamNs_.load(std::memory_order_relaxed);
	const FrameClock clock(drain_->rate);
	if (!discarding_) {
		// What the feed holds will not be read in its time: consumed now, counted with the drops so the library is
		// told it in order with them (told later, by the reader, the library would correct twice: once for the drops
		// it was told without it, once for it). Counted before it is marked stale, so the reader takes back only a drop
		// that has begun.
		const ReaderMark mark = readerMark();
		const std::uint32_t to = ring_.writePosition();
		staleFrom_.store(mark.from, std::memory_order_relaxed);
		staleTo_.store(to, std::memory_order_relaxed);
		staleConsumed_.store(mark.consumed, std::memory_order_relaxed);
		staleClears_.store(mark.clears, std::memory_order_relaxed);
		lastStaleFrames_.store(to - mark.from, std::memory_order_release);
		publishDrop(to - mark.from, now + downstream, true);
		stale_.store(true, std::memory_order_release);
		discarding_ = true;
		paceRawNs_ = now;
	}
	// A writer that was away is credited with a feed's worth of the time it missed at most, as a reader that kept
	// reading would have drained no more than the feed held.
	paceRawNs_ = std::max(paceRawNs_, now - clock.ns(ring_.capacityFrames()));
	const std::int64_t room = clock.frames(std::chrono::nanoseconds{now - paceRawNs_}); // whole frames, up to now
	if (room <= 0) {
		// Until a frame's time has come, at most its patience.
		const auto untilRoom =
			std::chrono::ceil<std::chrono::milliseconds>(std::chrono::nanoseconds{clock.ns(1) - (now - paceRawNs_)});
		waitForRoom(std::min(patience, untilRoom));
		return 0;
	}
	const auto accepted = static_cast<std::uint32_t>(std::min<std::int64_t>(frames, room));
	const std::int64_t pace = paceRawNs_ + clock.ns(accepted);
	if (!publishDrop(accepted, pace + downstream, false)) {
		discarding_ = false;
		return 0;
	}
	paceRawNs_ = pace;
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
