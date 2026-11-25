// The sinks (ThreadSinks; docs/engine.md#sinks): the realtime thread to an ordinary one, JACK's disk-writer pattern.
// The block's tap writes into core's SinkRing (every rule of a sink's two rings, which the type harness's sink wraps
// too) and wakes the sink's own pw_thread_loop with pw_loop_signal_event; that thread, where the consumer began, hands
// it what is in the ring. A full ring drops frames and counts them.
#include "ear/pipewire/sinks.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <pipewire/pipewire.h>

#include "callback_boundary.hpp"
#include "ear/core/sink_ring.hpp"

namespace ear::pipewire {
namespace {

/// A thread loop held by its owner, declared before the sources on it, so a constructor that throws after making it
/// destroys it (after those sources), never leaks it (docs/architecture.md#faults-by-owner).
struct ThreadLoopDestroy {
	void operator()(pw_thread_loop* loop) const noexcept { pw_thread_loop_destroy(loop); }
};
using OwnedThreadLoop = std::unique_ptr<pw_thread_loop, ThreadLoopDestroy>;

/// One sink: a wake and a thread around a SinkRing, so the sink's thread hands its consumer one
/// stamped chunk per cycle the tap wrote, in two calls where it wraps the ring's end (docs/engine.md#sinks). Every
/// `begun` and `closed` is posted through the main-loop Executor, after the consumer's begin or finish returned, so a
/// lost() its consumer said inside begin() (the node's handle, the same FIFO) always lands first.
class ThreadSink final : public Sink, public SinkTap {
public:
	ThreadSink(Executor& mainLoop, const NodeName& node, std::string_view bank, std::uint32_t channels,
	           std::uint32_t capacityFrames, SampleRate rate)
		: mainLoop_(mainLoop), rules_(channels, capacityFrames, rate) {
		const std::string name = node + "." + std::string(bank); // its engine name
		thread_.reset(pw_thread_loop_new(name.c_str(), nullptr));
		if (thread_ == nullptr) throw std::system_error(errno, std::generic_category(), "sink thread " + name);
		addEvent(wake_, pw_thread_loop_get_loop(thread_.get()), guarded<&ThreadSink::onWake>, this);
		if (!wake_) {
			const int error = errno;
			destroy();
			throw std::system_error(error, std::generic_category(), "sink thread " + name);
		}
		if (const int result = pw_thread_loop_start(thread_.get()); result < 0) {
			destroy();
			throw std::system_error(-result, std::generic_category(), "sink thread " + name);
		}
	}

	~ThreadSink() override { destroy(); }

	ThreadSink(const ThreadSink&) = delete;
	ThreadSink& operator=(const ThreadSink&) = delete;

	SinkTap& tap() noexcept override { return *this; }

	void begin(std::unique_ptr<SinkConsumer> consumer, std::move_only_function<void()> begun) override {
		consumer_ = std::move(consumer);
		begun_ = std::move(begun);
		beginAsked_.store(true, std::memory_order_release);
		signal();
	}

	void close(std::move_only_function<void(std::uint64_t droppedFrames)> closed) override {
		closed_ = std::move(closed);
		// The block can no longer run a code that writes the tap, so every write happened before this store.
		closing_.store(true, std::memory_order_release);
		signal();
	}

	std::uint64_t droppedFrames() const noexcept override { return rules_.droppedFrames(); }
	SinkControl& control() noexcept override { return pause_; }

	/// Realtime: the two rings drop together (SinkRing::write); the thread is woken when a frame was taken.
	void write(std::span<const std::span<const float>> bank, std::uint32_t frames,
	           const ChunkStamp& stamp) noexcept override {
		if (rules_.write(bank, frames, stamp)) signal();
	}

private:
	void signal() noexcept { pw_loop_signal_event(pw_thread_loop_get_loop(thread_.get()), wake_.get()); }

	/// The sink's thread: begin once asked, every stamped chunk to the consumer, then (closing) finish once.
	void onWake(std::uint64_t /*count*/) {
		// Read before draining: once closing, the drain below sees every write.
		const bool closing = closing_.load(std::memory_order_acquire);
		if (!began_ && !finished_ && beginAsked_.load(std::memory_order_acquire)) {
			began_ = true;
			consumer_->begin();
			mainLoop_.post(std::move(begun_));
		}
		static_cast<void>(pause_.holds()); // a pause asked while no chunk comes is taken here
		drain();
		if (closing && !finished_) {
			finished_ = true;
			if (began_) consumer_->finish(); // only a consumer that began
			mainLoop_.post(
				[closed = std::move(closed_), dropped = rules_.droppedFrames()]() mutable { closed(dropped); });
		}
	}

	/// Every stamped chunk to a begun consumer, in order, but while it is paused (each chunk then discarded and
	/// counted: SinkPause, taken between two chunks); frames that no begun consumer can take (none began, or it
	/// finished) are skipped.
	void drain() {
		if (began_ && !finished_)
			rules_.drain([this](const Chunk& chunk) {
				if (pause_.holds()) return pause_.discarded(chunk.frames);
				consumer_->consume(chunk);
			});
		else rules_.discard();
	}

	/// Stops the thread (joining it) before anything it reads goes.
	void destroy() noexcept {
		if (thread_ == nullptr) return;
		pw_thread_loop_stop(thread_.get());
		wake_.reset();
		thread_.reset();
	}

	Executor& mainLoop_;
	SinkRing rules_;
	SinkPause pause_{[this] { signal(); }}; // its wake: the thread takes a request with no chunk coming
	std::atomic<bool> beginAsked_{false};
	std::atomic<bool> closing_{false};
	std::unique_ptr<SinkConsumer> consumer_;              // set before beginAsked_; then the sink's thread's
	std::move_only_function<void()> begun_;               // likewise
	std::move_only_function<void(std::uint64_t)> closed_; // set before closing_; then the sink's thread's
	bool began_ = false;                                  // the sink's thread only
	bool finished_ = false;                               // the sink's thread only
	OwnedThreadLoop thread_;
	LoopSource wake_; // on the sink's thread loop
};

} // namespace

ThreadSinks::ThreadSinks(Executor& mainLoop) : mainLoop_(mainLoop) {}

std::unique_ptr<Sink> ThreadSinks::make(const NodeName& node, std::string_view bank, std::uint32_t channels,
                                        std::uint32_t capacityFrames, SampleRate rate) {
	return std::make_unique<ThreadSink>(mainLoop_, node, bank, channels, capacityFrames, rate);
}

} // namespace ear::pipewire
