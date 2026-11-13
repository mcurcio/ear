// The source companions (ear/pipewire/sources.hpp; docs/writing-a-node-type.md#sources): core's SourceRing, which holds
// every rule, wrapped with what only a thread can do: the writer's wait for room (poll on an eventfd that play()
// writes only while a writer waits, and close() writes once) and the clock wake's eventfd; and the source's clock,
// owned with its ring and handed to SourceRing::play each tick. The eventfd wake moves to ear_runtime's EventWake in
// slice 3c.
#include "ear/pipewire/sources.hpp"

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <chrono>
#include <system_error>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "ear/core/source_ring.hpp"

namespace ear::pipewire {
namespace {

/// The longest a single wait is taken to last: a patience of milliseconds::max() would overflow now() + patience.
constexpr std::chrono::milliseconds longestPatience = std::chrono::hours(24 * 365);

/// When a wait of `patience` from now ends, the patience clamped to [0, a year].
std::chrono::steady_clock::time_point deadlineAfter(std::chrono::milliseconds patience) noexcept {
	return std::chrono::steady_clock::now() + std::clamp(patience, std::chrono::milliseconds(0), longestPatience);
}

/// An eventfd: made non-blocking and close-on-exec, written (one write never blocks: realtime-safe), drained.
class EventFd {
public:
	EventFd() : fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
		if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "eventfd");
	}
	~EventFd() { ::close(fd_); }
	EventFd(const EventFd&) = delete;
	EventFd& operator=(const EventFd&) = delete;
	int fd() const noexcept { return fd_; }
	void ring() const noexcept {
		const std::uint64_t one = 1;
		[[maybe_unused]] const auto ignored = ::write(fd_, &one, sizeof one);
	}
	void drain() const noexcept {
		std::uint64_t rings = 0;
		[[maybe_unused]] const auto ignored = ::read(fd_, &rings, sizeof rings);
	}
	/// Waits until it is written or `until` passes (at most `until`, whatever wakes it early), then drains it. True
	/// if it was written.
	bool waitUntil(std::chrono::steady_clock::time_point until) const noexcept {
		const auto left = std::chrono::ceil<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
		if (left.count() <= 0) return false;
		pollfd ready{fd_, POLLIN, 0};
		const int got =
			::poll(&ready, 1, static_cast<int>(std::min<std::chrono::milliseconds::rep>(left.count(), INT_MAX)));
		drain();
		return got == 1 && (ready.revents & POLLIN) != 0;
	}

private:
	int fd_;
};

/// Wake on an eventfd: written once until answered.
class EventWake final : public Wake {
public:
	void ring() noexcept override {
		if (!pending_.exchange(true, std::memory_order_acq_rel)) fd_.ring();
	}
	int descriptor() const noexcept override { return fd_.fd(); }
	void answer() noexcept override {
		fd_.drain();
		// acq_rel: the watcher's look that follows sees what the ring published before it rang.
		pending_.exchange(false, std::memory_order_acq_rel);
	}

private:
	EventFd fd_;
	std::atomic<bool> pending_{false};
};

class EventSource final : public Source, public SourceWriter, public SourceTap, private SourceRing::Signals {
public:
	EventSource(std::uint32_t channels, std::uint32_t capacityFrames, std::uint32_t rate,
	            std::unique_ptr<SourceClock> clock)
		: clock_(std::move(clock)), ring_(channels, capacityFrames, rate, *this) {}

	// Source.
	SourceWriter& writer() noexcept override { return *this; }
	SourceTap& tap() noexcept override { return *this; }
	void stopped() noexcept override { ring_.dropForStop(); }
	void stopping() noexcept override { ring_.markIdle(); }
	void close() noexcept override {
		if (ring_.closed()) return;
		ring_.close();
		room_.ring(); // a write or idle waiting wakes, sees it closed and returns
	}
	std::uint64_t shortfallFrames() const noexcept override { return ring_.shortfallFrames(); }
	std::uint64_t droppedFrames() const noexcept override { return ring_.droppedFrames(); }

	// SourceWriter.
	std::uint32_t write(std::span<const float> interleaved, std::uint32_t frames,
	                    std::chrono::milliseconds patience) override {
		const auto until = deadlineAfter(patience);
		for (;;) {
			if (ring_.closed()) return sleepClosed(until);
			if (const std::uint32_t accepted = ring_.offer(interleaved, frames); accepted > 0 || frames == 0)
				return accepted;
			// Say it waits, then look again: play() frees room, then looks. Each fence puts one thread's look after the
			// other's change, so a wake is never lost.
			writerWaits_.store(true, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			if (const std::uint32_t accepted = ring_.offer(interleaved, frames); accepted > 0) {
				writerWaits_.store(false, std::memory_order_relaxed);
				return accepted;
			}
			if (ring_.closed() || std::chrono::steady_clock::now() >= until) {
				writerWaits_.store(false, std::memory_order_relaxed);
				return 0;
			}
			waitRoom(until);
			writerWaits_.store(false, std::memory_order_relaxed);
			// Closed while it waited: at once, accepting nothing (the loop's next look returns).
			if (ring_.closed()) return 0;
		}
	}
	void idle(std::chrono::milliseconds patience) override {
		const auto until = deadlineAfter(patience);
		if (ring_.closed()) {
			sleepClosed(until);
			return;
		}
		ring_.markIdle();
		// Nothing but a close writes the room descriptor while no writer says it waits; a stale write from before
		// wakes it early once, and it waits again.
		while (!ring_.closed() && std::chrono::steady_clock::now() < until) {
			waitRoom(until);
		}
	}
	void clear() noexcept override { ring_.clear(); }
	bool closed() const noexcept override { return ring_.closed(); }
	std::uint32_t capacityFrames() const noexcept override { return ring_.capacityFrames(); }
	Wake& clockWake() noexcept override { return clockWake_; }

	// SourceTap: the realtime thread, once per tick a code runs: the one play rule, with the source's clock.
	void play(std::span<const std::span<float>> bank, const SourceTick& tick) noexcept override {
		ring_.play(bank, tick, clock_.get());
	}

	/// EventSources::waitsBegun and waitsWoken.
	std::uint64_t waitsBegun() const noexcept { return waitsBegun_.load(std::memory_order_acquire); }
	std::uint64_t waitsWoken() const noexcept { return waitsWoken_.load(std::memory_order_acquire); }

private:
	// SourceRing::Signals, inside play() on the realtime thread: at most one eventfd write each.
	void roomFreed() noexcept override {
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (writerWaits_.exchange(false, std::memory_order_relaxed)) room_.ring();
	}
	void clockAsked() noexcept override { clockWake_.ring(); }

	/// A call after the close: accepts nothing and sleeps its whole patience, so a library thread calling in a loop
	/// never spins. Only close() wrote the descriptor after the close, once; it is drained and slept on again.
	std::uint32_t sleepClosed(std::chrono::steady_clock::time_point until) const noexcept {
		while (std::chrono::steady_clock::now() < until) {
			waitRoom(until);
		}
		return 0;
	}
	/// The writer's thread: waits on the room descriptor, past its last look at the ring, so only a write of the
	/// descriptor (play freeing room, or the close) or its patience ends the wait; counted for the test seams.
	void waitRoom(std::chrono::steady_clock::time_point until) const noexcept {
		waitsBegun_.fetch_add(1, std::memory_order_release);
		if (room_.waitUntil(until)) waitsWoken_.fetch_add(1, std::memory_order_release);
	}

	EventFd room_;
	std::atomic<bool> writerWaits_{false};
	mutable std::atomic<std::uint64_t> waitsBegun_{0}; // test seams: EventSources::waitsBegun and waitsWoken
	mutable std::atomic<std::uint64_t> waitsWoken_{0};
	EventWake clockWake_;
	std::unique_ptr<SourceClock> clock_; // the source's own (null: none), freed with it
	SourceRing ring_;                    // last: its Signals are the members above
};

} // namespace

std::unique_ptr<Source> EventSources::make(const NodeName& node, std::string_view bank, std::uint32_t channels,
                                           std::uint32_t capacityFrames, std::uint32_t rate,
                                           std::unique_ptr<SourceClock> clock) {
	static_cast<void>(node);
	static_cast<void>(bank);
	return std::make_unique<EventSource>(channels, capacityFrames, rate, std::move(clock));
}

std::uint64_t EventSources::waitsBegun(const Source& source) noexcept {
	const auto* made = dynamic_cast<const EventSource*>(&source);
	return made != nullptr ? made->waitsBegun() : 0;
}

std::uint64_t EventSources::waitsWoken(const Source& source) noexcept {
	const auto* made = dynamic_cast<const EventSource*>(&source);
	return made != nullptr ? made->waitsWoken() : 0;
}

std::chrono::steady_clock::time_point EventSources::waitDeadline(std::chrono::milliseconds patience) noexcept {
	return deadlineAfter(patience);
}

} // namespace ear::pipewire
