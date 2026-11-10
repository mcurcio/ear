// The realtime to ordinary-thread handoff (the legacy handoff `sendspin` still uses). JACK's disk-writer pattern: the
// realtime writer interleaves into a FrameRing and wakes the handoff's own pw_thread_loop with pw_loop_signal_event;
// that thread hands what is in the ring to the consumer. A full ring drops frames and counts them. The interim sinks
// (ThreadSinks; docs/engine.md#sinks) are the same wake and thread around core's SinkRing (every rule of a sink's two
// rings, which the type harness's sink wraps too), a consumer that begins on the sink's thread, and the tap the port
// block writes.
#include "ear/pipewire/handoffs.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <string>
#include <system_error>
#include <utility>

#include <pipewire/pipewire.h>

#include "callback_boundary.hpp"
#include "ear/core/frame_ring.hpp"
#include "ear/core/sink_ring.hpp"

namespace ear::pipewire {
namespace {

class ThreadHandoff final : public Handoff, public RtWriter {
public:
	ThreadHandoff(pw_loop* mainLoop, const NodeName& node, std::uint32_t channels, std::uint32_t capacityFrames,
	              HandoffConsumer& consumer)
		: mainLoop_(mainLoop), ring_(channels, capacityFrames), consumer_(consumer) {
		const std::string name = "ear-handoff " + node;
		thread_ = pw_thread_loop_new(name.c_str(), nullptr);
		if (thread_ == nullptr) throw std::system_error(errno, std::generic_category(), name);
		addEvent(wake_, pw_thread_loop_get_loop(thread_), guarded<&ThreadHandoff::onWake>, this);
		addEvent(finished_, mainLoop_, guarded<&ThreadHandoff::onFinished>, this);
		if (const int result = pw_thread_loop_start(thread_); result < 0) {
			destroy();
			throw std::system_error(-result, std::generic_category(), name);
		}
	}

	~ThreadHandoff() override { destroy(); }

	ThreadHandoff(const ThreadHandoff&) = delete;
	ThreadHandoff& operator=(const ThreadHandoff&) = delete;

	RtWriter& writer() noexcept override { return *this; }

	void close(std::move_only_function<void(std::uint64_t droppedFrames)> closed) override {
		closed_ = std::move(closed);
		// The writing port block is gone, so every write happened before this store.
		closing_.store(true, std::memory_order_release);
		pw_loop_signal_event(pw_thread_loop_get_loop(thread_), wake_.get());
	}

	std::uint32_t write(std::span<const std::span<const float>> channels, std::uint32_t frames) noexcept override {
		const std::uint32_t accepted = ring_.writePlanar(channels, frames);
		if (accepted < frames) dropped_.fetch_add(frames - accepted, std::memory_order_relaxed);
		if (accepted > 0) pw_loop_signal_event(pw_thread_loop_get_loop(thread_), wake_.get());
		return accepted;
	}

private:
	/// The handoff's thread: everything in the ring to the consumer, in write order.
	void drain() {
		for (;;) {
			// Up to the ring's end; the rest (wrapped to its start) on the next pass.
			const std::span<const float> run = ring_.readSpan();
			if (run.empty()) return;
			const auto frames = static_cast<std::uint32_t>(run.size() / ring_.channels());
			consumer_.consume(run, frames);
			ring_.skip(frames);
		}
	}

	void onWake(std::uint64_t /*count*/) {
		// Read before draining: once closing, the drain below sees every write.
		const bool closing = closing_.load(std::memory_order_acquire);
		drain();
		if (closing && !consumerFinished_) {
			consumerFinished_ = true;
			consumer_.finish();
			pw_loop_signal_event(mainLoop_, finished_.get());
		}
	}

	/// The main loop, after the consumer's finish(). The handoff's thread, idle now, is
	/// stopped by the destructor, never here: `closed` must not wait on the consumer. `closed`
	/// may destroy this handoff, so it is the last thing done here.
	void onFinished(std::uint64_t /*count*/) {
		if (!closed_) return;
		auto closed = std::exchange(closed_, {});
		closed(dropped_.load(std::memory_order_relaxed));
	}

	void destroy() noexcept {
		if (thread_ == nullptr) return;
		pw_thread_loop_stop(thread_);
		wake_.reset();
		pw_thread_loop_destroy(thread_);
		thread_ = nullptr;
		finished_.reset();
	}

	pw_loop* mainLoop_;
	FrameRing ring_;
	HandoffConsumer& consumer_;
	pw_thread_loop* thread_ = nullptr;
	LoopSource wake_;     // on the handoff's thread loop
	LoopSource finished_; // on the main loop
	std::atomic<std::uint64_t> dropped_{0};
	std::atomic<bool> closing_{false};
	bool consumerFinished_ = false;                       // handoff thread only
	std::move_only_function<void(std::uint64_t)> closed_; // main thread only
};

/// One interim sink: ThreadHandoff's wake and thread around a SinkRing, so the sink's thread hands its consumer one
/// stamped chunk per cycle the tap wrote, in two calls where it wraps the ring's end (docs/engine.md#sinks). Every
/// `begun` and `closed` is posted through the main-loop Executor, after the consumer's begin or finish returned, so a
/// lost() its consumer said inside begin() (the node's InsidesEvents, the same FIFO) always lands first.
class ThreadSink final : public Sink, public SinkTap {
public:
	ThreadSink(Executor& mainLoop, const NodeName& node, std::string_view bank, std::uint32_t channels,
	           std::uint32_t capacityFrames, std::uint32_t rate)
		: mainLoop_(mainLoop), rules_(channels, capacityFrames, rate) {
		const std::string name = node + "." + std::string(bank); // its engine name
		thread_ = pw_thread_loop_new(name.c_str(), nullptr);
		if (thread_ == nullptr) throw std::system_error(errno, std::generic_category(), "sink thread " + name);
		addEvent(wake_, pw_thread_loop_get_loop(thread_), guarded<&ThreadSink::onWake>, this);
		if (!wake_) {
			const int error = errno;
			destroy();
			throw std::system_error(error, std::generic_category(), "sink thread " + name);
		}
		if (const int result = pw_thread_loop_start(thread_); result < 0) {
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

	/// Realtime: the two rings drop together (SinkRing::write); the thread is woken when a frame was taken.
	void write(std::span<const std::span<const float>> bank, std::uint32_t frames,
	           const ChunkStamp& stamp) noexcept override {
		if (rules_.write(bank, frames, stamp)) signal();
	}

private:
	void signal() noexcept { pw_loop_signal_event(pw_thread_loop_get_loop(thread_), wake_.get()); }

	/// The sink's thread: begin once asked, every stamped chunk to the consumer, then (closing) finish once.
	void onWake(std::uint64_t /*count*/) {
		// Read before draining: once closing, the drain below sees every write.
		const bool closing = closing_.load(std::memory_order_acquire);
		if (!began_ && !finished_ && beginAsked_.load(std::memory_order_acquire)) {
			began_ = true;
			consumer_->begin();
			mainLoop_.post(std::move(begun_));
		}
		drain();
		if (closing && !finished_) {
			finished_ = true;
			if (began_) consumer_->finish(); // only a consumer that began
			mainLoop_.post(
				[closed = std::move(closed_), dropped = rules_.droppedFrames()]() mutable { closed(dropped); });
		}
	}

	/// Every stamped chunk to a begun consumer, in order; frames that no begun consumer can take (none began, or it
	/// finished) are skipped.
	void drain() {
		if (began_ && !finished_) rules_.drain([this](const Chunk& chunk) { consumer_->consume(chunk); });
		else rules_.discard();
	}

	/// Stops the thread (joining it) before anything it reads goes.
	void destroy() noexcept {
		if (thread_ == nullptr) return;
		pw_thread_loop_stop(thread_);
		wake_.reset();
		pw_thread_loop_destroy(thread_);
		thread_ = nullptr;
	}

	Executor& mainLoop_;
	SinkRing rules_;
	std::atomic<bool> beginAsked_{false};
	std::atomic<bool> closing_{false};
	std::unique_ptr<SinkConsumer> consumer_;              // set before beginAsked_; then the sink's thread's
	std::move_only_function<void()> begun_;               // likewise
	std::move_only_function<void(std::uint64_t)> closed_; // set before closing_; then the sink's thread's
	bool began_ = false;                                  // the sink's thread only
	bool finished_ = false;                               // the sink's thread only
	pw_thread_loop* thread_ = nullptr;
	LoopSource wake_; // on the sink's thread loop
};

} // namespace

ThreadHandoffs::ThreadHandoffs(pw_loop* mainLoop) : mainLoop_(mainLoop) {}

std::unique_ptr<Handoff> ThreadHandoffs::make(const NodeName& node, std::uint32_t channels,
                                              std::uint32_t capacityFrames, HandoffConsumer& consumer) {
	return std::make_unique<ThreadHandoff>(mainLoop_, node, channels, capacityFrames, consumer);
}

ThreadSinks::ThreadSinks(Executor& mainLoop) : mainLoop_(mainLoop) {}

std::unique_ptr<Sink> ThreadSinks::make(const NodeName& node, std::string_view bank, std::uint32_t channels,
                                        std::uint32_t capacityFrames, std::uint32_t rate) {
	return std::make_unique<ThreadSink>(mainLoop_, node, bank, channels, capacityFrames, rate);
}

} // namespace ear::pipewire
