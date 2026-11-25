// A sink's pause (SinkPause, docs/writing-a-node-type.md#pausing-a-sink), with real threads around a SinkRing as
// ear_pipewire's ThreadSink runs it: the sink's thread finishes a consume in flight before `paused` is posted, then
// discards and counts until resume, and the next chunk after it is consumed; a writer at full rate while pausing and
// resuming 10^4 times never sees a consume between `paused` and resume, and every frame is consumed, paused or dropped
// (the pause read only between chunks, never on the tap's path). Every wait is event-gated: the deadline only fails a
// test.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <array>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/sink_ring.hpp"
#include "sleeps.hpp"

namespace {
using namespace ear;
using namespace std::chrono_literals;

constexpr auto kDeadline = 10s;

/// A loop that only posts: what is posted waits until the test runs it (run), on the test's thread.
class PostLoop final : public Loop {
public:
	void post(std::move_only_function<void()> work) override {
		{
			const std::scoped_lock lock(mutex_);
			queue_.push_back(std::move(work));
		}
		changed_.notify_all();
	}
	/// Runs one posted work, waiting for it; false at the deadline.
	bool runOne() {
		std::move_only_function<void()> next;
		{
			std::unique_lock lock(mutex_);
			if (!changed_.wait_for(lock, kDeadline, [&] { return !queue_.empty(); })) return false;
			next = std::move(queue_.front());
			queue_.pop_front();
		}
		next();
		return true;
	}
	std::size_t queued() {
		const std::scoped_lock lock(mutex_);
		return queue_.size();
	}
	std::unique_ptr<Timer> after(std::chrono::milliseconds, std::move_only_function<void()>) override {
		throw std::logic_error("no timers");
	}
	std::unique_ptr<Timer> every(std::chrono::milliseconds, std::move_only_function<void()>) override {
		throw std::logic_error("no timers");
	}
	std::unique_ptr<Timer> onWake(Wake&, std::move_only_function<void()>) override {
		throw std::logic_error("no watches");
	}
	std::unique_ptr<Timer> onReadable(int, std::move_only_function<void()>) override {
		throw std::logic_error("no watches");
	}
	LoopKind kind() const noexcept override { return LoopKind::dedicated; }
	bool inLoop() const noexcept override { return false; }

private:
	std::mutex mutex_;
	std::condition_variable changed_;
	std::deque<std::move_only_function<void()>> queue_;
};

/// The sink's thread as ThreadSink runs it: woken by the tap's write or the pause's wake, it takes a pause request,
/// then drains, each chunk discarded while paused, else consumed.
class SinkThread {
public:
	SinkThread(std::uint32_t frames, std::move_only_function<void(const Chunk&)> consume)
		: ring(1, frames, SampleRate::fixed<48000>()), consume_(std::move(consume)), thread_([this] { run(); }) {}
	~SinkThread() {
		stop_.store(true);
		wake();
		thread_.join();
	}
	void wake() {
		{
			const std::scoped_lock lock(mutex_);
			woken_ = true;
		}
		changed_.notify_one();
	}
	SinkRing ring;
	SinkPause pause{[this] { wake(); }};

private:
	void run() {
		while (!stop_.load()) {
			{
				std::unique_lock lock(mutex_);
				changed_.wait(lock, [&] { return woken_ || stop_.load(); });
				woken_ = false;
			}
			static_cast<void>(pause.holds());
			ring.drain([&](const Chunk& chunk) {
				if (pause.holds()) return pause.discarded(chunk.frames);
				consume_(chunk);
			});
		}
	}
	std::move_only_function<void(const Chunk&)> consume_;
	std::mutex mutex_;
	std::condition_variable changed_;
	bool woken_ = false;
	std::atomic<bool> stop_{false};
	std::thread thread_; // last: it reads everything above
};

/// One mono chunk of `frames` into the ring, at `position`.
bool write(SinkThread& sink, std::uint32_t frames, std::uint64_t position) {
	std::vector<float> samples(frames, 0.5F);
	const std::span<const float> member(samples);
	const std::array<std::span<const float>, 1> bank{member};
	const bool taken = sink.ring.write(bank, frames, ChunkStamp{.position = position});
	if (taken) sink.wake();
	return taken;
}

bool eventually(const std::function<bool()>& done) {
	const auto deadline = std::chrono::steady_clock::now() + kDeadline;
	while (!done()) {
		if (std::chrono::steady_clock::now() > deadline) return false;
		std::this_thread::yield();
	}
	return true;
}

} // namespace

TEST_CASE("A sink's pause waits for a consume in flight, then discards and counts chunks until resume, and posts "
          "paused once no consume can run",
          "[sink][pause]") {
	std::mutex gate;
	std::unique_lock held(gate); // the first consume waits on it
	std::atomic<int> consumes{0};
	std::atomic<bool> inConsume{false};
	std::vector<std::uint64_t> positions;
	SinkThread sink(1024, [&](const Chunk& chunk) {
		inConsume.store(true);
		const std::scoped_lock wait(gate);
		positions.push_back(chunk.position);
		++consumes;
		inConsume.store(false);
	});
	PostLoop loop;
	REQUIRE(write(sink, 64, 0));
	REQUIRE(eventually([&] { return inConsume.load(); })); // a consume in flight
	bool paused = false;
	sink.pause.pause(loop, [&] { paused = true; });
	sleeps::negative(20ms, "a paused posted while the consume still runs would land within this");
	CHECK(loop.queued() == 0); // not while the consume runs
	held.unlock();
	REQUIRE(loop.runOne());
	CHECK(paused);
	CHECK_FALSE(inConsume.load());
	REQUIRE(write(sink, 32, 64));
	REQUIRE(eventually([&] { return sink.pause.pausedFrames() == 32; }));
	CHECK(consumes.load() == 1);
	bool again = false;
	sink.pause.pause(loop, [&] { again = true; }); // a paused sink posts at once
	REQUIRE(loop.runOne());
	CHECK(again);
	sink.pause.resume();
	REQUIRE(write(sink, 16, 96));
	REQUIRE(eventually([&] { return consumes.load() == 2; }));
	CHECK(positions == std::vector<std::uint64_t>{0, 96}); // the consumer sees the gap by the position
	CHECK(sink.ring.droppedFrames() == 0);
}

TEST_CASE("A sink paused and resumed 10^4 times under a writer at full rate never consumes between paused and resume, "
          "and every frame is consumed, paused or dropped",
          "[sink][pause][stress]") {
	std::atomic<bool> handedOver{false}; // between paused and resume: the loop holds the consumer
	std::atomic<std::uint64_t> consumed{0};
	std::atomic<int> violations{0};
	SinkThread sink(4096, [&](const Chunk& chunk) {
		if (handedOver.load()) ++violations;
		consumed += chunk.frames;
	});
	std::atomic<bool> writing{true};
	std::atomic<std::uint64_t> written{0};
	std::thread writer([&] {
		std::uint64_t position = 0;
		while (writing.load()) {
			static_cast<void>(write(sink, 64, position));
			position += 64;
			written += 64;
		}
	});
	PostLoop loop;
	constexpr int rounds = 10'000;
	int answered = 0;
	for (int i = 0; i < rounds; ++i) {
		bool paused = false;
		sink.pause.pause(loop, [&] { paused = true; });
		if (!loop.runOne() || !paused) break;
		handedOver.store(true); // the consumer is the loop's now: no consume may run
		for (int look = 0; look < 4; ++look) std::this_thread::yield();
		handedOver.store(false);
		sink.pause.resume();
		++answered;
	}
	writing.store(false);
	writer.join();
	CHECK(answered == rounds);
	CHECK(violations.load() == 0);
	REQUIRE(eventually(
		[&] { return consumed.load() + sink.pause.pausedFrames() + sink.ring.droppedFrames() == written.load(); }));
}
