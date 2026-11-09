// The realtime to ordinary-thread handoff (the legacy handoff `sendspin` still uses). JACK's disk-writer pattern: the
// realtime writer interleaves into a FrameRing and wakes the handoff's own pw_thread_loop with pw_loop_signal_event;
// that thread hands what is in the ring to the consumer. A full ring drops frames and counts them. The interim sinks
// (ThreadSinks; docs/engine.md#sinks) are the same ring, wake and thread, with a stamp ring beside the frame ring, a
// consumer that begins on the sink's thread, and the tap the port block writes.
#include "ear/pipewire/handoffs.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <string>
#include <vector>
#include <system_error>
#include <utility>

#include <pipewire/pipewire.h>

#include "ear/core/frame_ring.hpp"

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
		wake_ = pw_loop_add_event(pw_thread_loop_get_loop(thread_), &ThreadHandoff::onWake, this);
		finished_ = pw_loop_add_event(mainLoop_, &ThreadHandoff::onFinished, this);
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
		pw_loop_signal_event(pw_thread_loop_get_loop(thread_), wake_);
	}

	std::uint32_t write(std::span<const std::span<const float>> channels, std::uint32_t frames) noexcept override {
		const std::uint32_t accepted = ring_.writePlanar(channels, frames);
		if (accepted < frames) dropped_.fetch_add(frames - accepted, std::memory_order_relaxed);
		if (accepted > 0) pw_loop_signal_event(pw_thread_loop_get_loop(thread_), wake_);
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

	static void onWake(void* data, std::uint64_t /*count*/) {
		auto* self = static_cast<ThreadHandoff*>(data);
		// Read before draining: once closing, the drain below sees every write.
		const bool closing = self->closing_.load(std::memory_order_acquire);
		self->drain();
		if (closing && !self->consumerFinished_) {
			self->consumerFinished_ = true;
			self->consumer_.finish();
			pw_loop_signal_event(self->mainLoop_, self->finished_);
		}
	}

	/// The main loop, after the consumer's finish(). The handoff's thread, idle now, is
	/// stopped by the destructor, never here: `closed` must not wait on the consumer. `closed`
	/// may destroy this handoff, so it is the last thing done here.
	static void onFinished(void* data, std::uint64_t /*count*/) {
		auto* self = static_cast<ThreadHandoff*>(data);
		if (!self->closed_) return;
		auto closed = std::exchange(self->closed_, {});
		closed(self->dropped_.load(std::memory_order_relaxed));
	}

	void destroy() noexcept {
		if (thread_ == nullptr) return;
		pw_thread_loop_stop(thread_);
		if (wake_ != nullptr) pw_loop_destroy_source(pw_thread_loop_get_loop(thread_), wake_);
		pw_thread_loop_destroy(thread_);
		thread_ = nullptr;
		if (finished_ != nullptr) pw_loop_destroy_source(mainLoop_, finished_);
	}

	pw_loop* mainLoop_;
	FrameRing ring_;
	HandoffConsumer& consumer_;
	pw_thread_loop* thread_ = nullptr;
	spa_source* wake_ = nullptr;     // on the handoff's thread loop
	spa_source* finished_ = nullptr; // on the main loop
	std::atomic<std::uint64_t> dropped_{0};
	std::atomic<bool> closing_{false};
	bool consumerFinished_ = false;                       // handoff thread only
	std::move_only_function<void(std::uint64_t)> closed_; // main thread only
};

/// One interim sink: ThreadHandoff's ring and thread, plus a stamp ring, so the sink's thread hands its consumer one
/// stamped chunk per cycle the tap wrote (docs/engine.md#sinks). Every `begun` and `closed` is posted through the
/// main-loop Executor, after the consumer's begin or finish returned, so a lost() its consumer said inside begin() (the
/// node's InsidesEvents, the same FIFO) always lands first.
class ThreadSink final : public Sink, public SinkTap {
public:
	ThreadSink(Executor& mainLoop, const NodeName& node, std::string_view bank, std::uint32_t channels,
	           std::uint32_t capacityFrames)
		: mainLoop_(mainLoop), ring_(channels, capacityFrames),
		  stamps_(std::bit_ceil(std::max<std::uint32_t>(64, ring_.capacityFrames() / 32))) {
		const std::string name = node + "." + std::string(bank); // its engine name
		thread_ = pw_thread_loop_new(name.c_str(), nullptr);
		if (thread_ == nullptr) throw std::system_error(errno, std::generic_category(), "sink thread " + name);
		wake_ = pw_loop_add_event(pw_thread_loop_get_loop(thread_), &ThreadSink::onWake, this);
		if (wake_ == nullptr) {
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

	std::uint64_t droppedFrames() const noexcept override { return dropped_.load(std::memory_order_relaxed); }

	/// Realtime: the two rings drop together (docs/engine.md#sinks). With no free stamp entry nothing is written and
	/// the whole chunk is dropped; else the frames the frame ring has room for are written and stamped.
	void write(std::span<const std::span<const float>> bank, std::uint32_t frames,
	           const ChunkStamp& stamp) noexcept override {
		const std::uint32_t written = stampsWritten_.load(std::memory_order_relaxed);
		if (written - stampsRead_.load(std::memory_order_acquire) >= stamps_.size()) {
			dropped_.fetch_add(frames, std::memory_order_relaxed);
			return;
		}
		const std::uint32_t accepted = ring_.writePlanar(bank, frames);
		if (accepted < frames) dropped_.fetch_add(frames - accepted, std::memory_order_relaxed);
		if (accepted == 0) return;
		stamps_[written & (stamps_.size() - 1)] = Stamped{stamp, accepted};
		stampsWritten_.store(written + 1, std::memory_order_release);
		signal();
	}

private:
	/// What the tap pushed beside one cycle's frames: its stamp and how many frames the frame ring took.
	struct Stamped {
		ChunkStamp stamp;
		std::uint32_t frames;
	};

	void signal() noexcept { pw_loop_signal_event(pw_thread_loop_get_loop(thread_), wake_); }

	/// The sink's thread: begin once asked, every stamped chunk to the consumer, then (closing) finish once.
	static void onWake(void* data, std::uint64_t /*count*/) {
		auto* self = static_cast<ThreadSink*>(data);
		// Read before draining: once closing, the drain below sees every write.
		const bool closing = self->closing_.load(std::memory_order_acquire);
		if (!self->began_ && !self->finished_ && self->beginAsked_.load(std::memory_order_acquire)) {
			self->began_ = true;
			self->consumer_->begin();
			self->mainLoop_.post(std::move(self->begun_));
		}
		self->drain();
		if (closing && !self->finished_) {
			self->finished_ = true;
			if (self->began_) self->consumer_->finish(); // only a consumer that began
			self->mainLoop_.post(
				[closed = std::move(self->closed_),
			     dropped = self->dropped_.load(std::memory_order_relaxed)]() mutable { closed(dropped); });
		}
	}

	/// Pairs each stamp with its frames, in order. Frames that no begun consumer can take (none began, or it finished)
	/// are skipped.
	void drain() {
		for (;;) {
			const std::uint32_t read = stampsRead_.load(std::memory_order_relaxed);
			if (read == stampsWritten_.load(std::memory_order_acquire)) return;
			const Stamped stamped = stamps_[read & (stamps_.size() - 1)];
			if (began_ && !finished_) consume(stamped);
			else ring_.skip(stamped.frames);
			stampsRead_.store(read + 1, std::memory_order_release);
		}
	}

	/// One chunk, in two calls when its frames wrap the ring's end: the second's position advanced by the first's
	/// frames and its discontinuity false (docs/engine.md#sinks).
	void consume(const Stamped& stamped) {
		std::uint32_t done = 0;
		while (done < stamped.frames) {
			const std::span<const float> run = ring_.readSpan();
			const std::uint32_t frames =
				std::min(stamped.frames - done, static_cast<std::uint32_t>(run.size() / ring_.channels()));
			if (frames == 0) return; // never: a chunk's frames are in the ring before its stamp
			consumer_->consume(Chunk{.interleaved = run.first(static_cast<std::size_t>(frames) * ring_.channels()),
			                         .frames = frames,
			                         .position = stamped.stamp.position + done,
			                         .startNs = stamped.stamp.startNs,
			                         .startRawNs = stamped.stamp.startRawNs,
			                         .upstreamFrames = stamped.stamp.upstreamFrames,
			                         .discontinuity = done == 0 && stamped.stamp.discontinuity});
			ring_.skip(frames);
			done += frames;
		}
	}

	/// Stops the thread (joining it) before anything it reads goes.
	void destroy() noexcept {
		if (thread_ == nullptr) return;
		pw_thread_loop_stop(thread_);
		if (wake_ != nullptr) pw_loop_destroy_source(pw_thread_loop_get_loop(thread_), wake_);
		pw_thread_loop_destroy(thread_);
		thread_ = nullptr;
	}

	Executor& mainLoop_;
	FrameRing ring_;
	std::vector<Stamped> stamps_; // a power of two entries
	alignas(64) std::atomic<std::uint32_t> stampsWritten_{0};
	alignas(64) std::atomic<std::uint32_t> stampsRead_{0};
	std::atomic<std::uint64_t> dropped_{0};
	std::atomic<bool> beginAsked_{false};
	std::atomic<bool> closing_{false};
	std::unique_ptr<SinkConsumer> consumer_;              // set before beginAsked_; then the sink's thread's
	std::move_only_function<void()> begun_;               // likewise
	std::move_only_function<void(std::uint64_t)> closed_; // set before closing_; then the sink's thread's
	bool began_ = false;                                  // the sink's thread only
	bool finished_ = false;                               // the sink's thread only
	pw_thread_loop* thread_ = nullptr;
	spa_source* wake_ = nullptr; // on the sink's thread loop
};

} // namespace

ThreadHandoffs::ThreadHandoffs(pw_loop* mainLoop) : mainLoop_(mainLoop) {}

std::unique_ptr<Handoff> ThreadHandoffs::make(const NodeName& node, std::uint32_t channels,
                                              std::uint32_t capacityFrames, HandoffConsumer& consumer) {
	return std::make_unique<ThreadHandoff>(mainLoop_, node, channels, capacityFrames, consumer);
}

ThreadSinks::ThreadSinks(Executor& mainLoop) : mainLoop_(mainLoop) {}

std::unique_ptr<Sink> ThreadSinks::make(const NodeName& node, std::string_view bank, std::uint32_t channels,
                                        std::uint32_t capacityFrames) {
	return std::make_unique<ThreadSink>(mainLoop_, node, bank, channels, capacityFrames);
}

} // namespace ear::pipewire
