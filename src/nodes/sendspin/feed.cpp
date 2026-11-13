#include "feed.hpp"

#include <algorithm>
#include <utility>

#include <poll.h>

namespace ear::sendspin {
namespace {

constexpr std::uint64_t clearPending = std::uint64_t{1} << 32U;

} // namespace

Feed::Feed(std::uint32_t channels, std::uint32_t capacityFrames) : ring_(channels, capacityFrames) {}

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

std::uint32_t Feed::read(std::span<const std::span<float>> channels, std::uint32_t frames) noexcept {
	const bool cleared = applyClear();
	const std::uint32_t got = ring_.readPlanar(channels, frames);
	for (const std::span<float> channel : channels) std::fill(channel.begin() + got, channel.begin() + frames, 0.0F);
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

Feed::Taken Feed::skipThenRead(std::uint64_t skip, std::uint64_t owed, std::span<const std::span<float>> channels,
                               std::uint32_t frames) noexcept {
	const bool cleared = applyClear();
	// What it holds now, on the reading thread, is exact: the writer only adds. The debt is paid from everything
	// readable: while some is still owed after a skip and the writer has added frames since, it skips again, so a frame
	// is never read, nor left for a later cycle, while the debt it could pay is owed. The read follows only once the
	// debt is paid; stale audio owed nothing (a starved library's) is skipped as far as the feed holds it.
	heldAtSkip_ = held();
	const std::uint64_t mustPay = std::min(owed, skip);
	std::uint64_t skipped = 0;
	for (std::uint32_t holds = heldAtSkip_; skipped < mustPay && holds > 0; holds = held()) {
		skipped += ring_.skip(static_cast<std::uint32_t>(std::min<std::uint64_t>(mustPay - skipped, holds)));
		if (skipHook_ != nullptr) skipHook_(skipHookContext_);
	}
	if (skipped >= mustPay && skip > skipped)
		skipped += ring_.skip(static_cast<std::uint32_t>(std::min<std::uint64_t>(skip - skipped, held())));
	const std::uint32_t got = skipped >= mustPay ? ring_.readPlanar(channels, frames) : 0;
	for (const std::span<float> channel : channels) std::fill(channel.begin() + got, channel.begin() + frames, 0.0F);
	if (cleared || skipped > 0 || got > 0) wakeWriter();
	return {static_cast<std::uint32_t>(std::min<std::uint64_t>(skipped, UINT32_MAX)), got};
}

bool Feed::takeCleared() noexcept {
	if (applyClear()) wakeWriter();
	return std::exchange(clearedSince_, false);
}

std::uint32_t Feed::tryWrite(std::span<const float> interleaved, std::uint32_t frames) noexcept {
	return writerHeld_.load(std::memory_order_acquire) ? 0 : ring_.writeInterleaved(interleaved, frames);
}

void Feed::holdWriter() noexcept { writerHeld_.store(true, std::memory_order_release); }

void Feed::releaseWriter() noexcept {
	writerHeld_.store(false, std::memory_order_release);
	wakeWriter();
}

std::uint32_t Feed::write(std::span<const float> interleaved, std::uint32_t frames,
                          std::chrono::milliseconds patience) {
	const auto until = std::chrono::steady_clock::now() + patience;
	for (;;) {
		if (const std::uint32_t accepted = tryWrite(interleaved, frames); accepted > 0 || frames == 0) {
			return accepted;
		}
		writerWaits_.store(true, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (const std::uint32_t accepted = tryWrite(interleaved, frames); accepted > 0) {
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

void Feed::clear() noexcept { clearTo_.store(clearPending | ring_.writePosition(), std::memory_order_release); }

} // namespace ear::sendspin
