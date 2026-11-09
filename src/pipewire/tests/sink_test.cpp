// Step L-4b's interim sinks (docs/design/node-lifecycle.md 5.11 and 11.6; node-lifecycle-modules.md 4.3, 4.4 and 4.7):
// the real ThreadSinks, first on its own thread with a test's main loop (order, stamps, a chunk wrapping the ring's end,
// the two rings dropping together, close draining then finishing, begin before the first consume and `begun` behind a
// lost said inside begin, no allocation on the tap), then through a real block on the in-process engine (a ramp
// recorded through the sink starts at the first driven sample and ends at the last, across a stop and a start, under
// an invoke storm). Every wait is event-gated: a test waits for the event it needs; the deadline only fails the test.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <pipewire/pipewire.h>

#include "allocation_count.hpp"
#include "catch2/catch.hpp"
#include "ear/pipewire/handoffs.hpp"
#include "ear/pipewire/main_loop_executor.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "engine_harness.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace std::chrono_literals;

constexpr auto kDeadline = 5s;

/// PipeWire initialised for the test (pw_thread_loop needs it); no engine.
struct PipeWire {
	PipeWire() { pw_init(nullptr, nullptr); }
	~PipeWire() { pw_deinit(); }
};

/// The main loop as a test turns it: posts from any thread wait here until the test runs them.
class TestLoop final : public Executor {
public:
	void post(std::move_only_function<void()> work) override {
		{
			const std::scoped_lock lock(mutex_);
			queue_.push_back(std::move(work));
		}
		changed_.notify_all();
	}
	/// Runs what is posted, as it comes, until `done` holds; false at the deadline.
	bool until(const std::function<bool()>& done) {
		const auto deadline = std::chrono::steady_clock::now() + kDeadline;
		while (!done()) {
			std::move_only_function<void()> next;
			{
				std::unique_lock lock(mutex_);
				if (!changed_.wait_until(lock, deadline, [&] { return !queue_.empty(); })) return done();
				next = std::move(queue_.front());
				queue_.pop_front();
			}
			next();
		}
		return true;
	}

private:
	std::mutex mutex_;
	std::condition_variable changed_;
	std::deque<std::move_only_function<void()>> queue_;
};

/// One chunk a consumer was handed, copied, with the thread it came on.
struct Got {
	std::vector<float> interleaved;
	std::uint32_t frames;
	std::uint64_t position;
	std::int64_t startNs;
	bool discontinuity;
	std::thread::id thread;
};

/// A one-way gate a consumer waits at; opening it twice is harmless.
class Gate {
public:
	void open() {
		{
			const std::scoped_lock lock(mutex_);
			open_ = true;
		}
		opened_.notify_all();
	}
	void wait() {
		std::unique_lock lock(mutex_);
		opened_.wait(lock, [&] { return open_; });
	}

private:
	std::mutex mutex_;
	std::condition_variable opened_;
	bool open_ = false;
};

/// Records every call, in order: "begin", each chunk, "finish". Optionally says lost (by posting it to the test's main
/// loop, as a node's InsidesEvents would) inside begin, and stalls in its first consume until released.
struct Shared {
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::string> calls;
	std::vector<Got> chunks;
	std::uint64_t frames = 0;
	bool stalled = false;
	Gate release;
	bool waitFor(const std::function<bool()>& done) {
		std::unique_lock lock(mutex);
		return changed.wait_for(lock, kDeadline, done);
	}
};
class TestConsumer final : public SinkConsumer {
public:
	TestConsumer(Shared& shared, TestLoop* loseTo = nullptr, bool stall = false)
		: shared_(shared), loseTo_(loseTo), stall_(stall) {}
	void begin() override {
		record("begin");
		if (loseTo_ != nullptr)
			loseTo_->post([&shared = shared_] {
				const std::scoped_lock lock(shared.mutex);
				shared.calls.emplace_back("lost");
			});
	}
	void consume(const Chunk& chunk) override {
		if (stall_ && !entered_) {
			entered_ = true;
			{
				const std::scoped_lock lock(shared_.mutex);
				shared_.stalled = true;
			}
			shared_.changed.notify_all();
			shared_.release.wait();
		}
		{
			const std::scoped_lock lock(shared_.mutex);
			shared_.calls.emplace_back("consume");
			shared_.chunks.push_back(Got{{chunk.interleaved.begin(), chunk.interleaved.end()},
			                             chunk.frames,
			                             chunk.position,
			                             chunk.startNs,
			                             chunk.discontinuity,
			                             std::this_thread::get_id()});
			shared_.frames += chunk.frames;
		}
		shared_.changed.notify_all();
	}
	void finish() override { record("finish"); }

private:
	void record(const char* call) {
		{
			const std::scoped_lock lock(shared_.mutex);
			shared_.calls.emplace_back(call);
		}
		shared_.changed.notify_all();
	}
	Shared& shared_;
	TestLoop* loseTo_;
	bool stall_;
	bool entered_ = false; // the sink's thread only
};

/// Planar test signal of two channels: frame n of channel c is n * 2 + c, so the interleaved stream counts up exactly
/// when frames arrive in order and channels in their order.
class Signal {
public:
	explicit Signal(std::uint32_t frames) : planes_(2, std::vector<float>(frames)), views_{planes_[0], planes_[1]} {}
	std::span<const std::span<const float>> block(std::uint64_t first, std::uint32_t frames) {
		for (std::uint32_t c = 0; c < 2; ++c)
			for (std::uint32_t f = 0; f < frames; ++f) planes_[c][f] = static_cast<float>((first + f) * 2 + c);
		return views_;
	}

private:
	std::vector<std::vector<float>> planes_;
	std::array<std::span<const float>, 2> views_;
};

ChunkStamp stampAt(std::uint64_t position, bool discontinuity = false) {
	return ChunkStamp{position, static_cast<std::int64_t>(position) * 1000, static_cast<std::int64_t>(position) * 1000,
	                  discontinuity};
}

/// Begins `sink` with `consumer` and waits for `begun` on the test's loop.
void beginAndWait(TestLoop& loop, Sink& sink, std::unique_ptr<SinkConsumer> consumer) {
	bool begun = false;
	sink.begin(std::move(consumer), [&] { begun = true; });
	REQUIRE(loop.until([&] { return begun; }));
}

/// Closes `sink` and runs the test's loop until `closed`; returns the dropped count.
std::uint64_t closeAndWait(TestLoop& loop, Sink& sink) {
	std::uint64_t dropped = ~0ULL;
	bool called = false;
	sink.close([&](std::uint64_t droppedFrames) {
		dropped = droppedFrames;
		called = true;
	});
	REQUIRE(loop.until([&] { return called; }));
	return dropped;
}

/// Every chunk's samples are the signal at its position, and no frame was handed twice or out of order.
void checkChunksAgainstSignal(const std::vector<Got>& chunks) {
	std::uint64_t next = 0;
	for (const Got& chunk : chunks) {
		INFO("chunk at " << chunk.position);
		REQUIRE(chunk.interleaved.size() == static_cast<std::size_t>(chunk.frames) * 2);
		REQUIRE(chunk.position >= next);
		for (std::uint32_t f = 0; f < chunk.frames; ++f)
			for (std::uint32_t c = 0; c < 2; ++c)
				REQUIRE(chunk.interleaved[f * 2 + c] == static_cast<float>((chunk.position + f) * 2 + c));
		next = chunk.position + chunk.frames;
	}
}

} // namespace

TEST_CASE("A sink's consumer begins on its own thread before the first consume, and its chunks arrive in order, "
          "stamped",
          "[sink]") {
	PipeWire pipewire;
	TestLoop loop;
	ThreadSinks sinks(loop);
	Shared shared;
	constexpr std::uint32_t kBlock = 64;
	constexpr std::uint32_t kBlocks = 100;
	auto sink = sinks.make("a-node-with-a-long-name", "writer", 2, kBlock * kBlocks);
	beginAndWait(loop, *sink, std::make_unique<TestConsumer>(shared));

	std::thread::id writerThread;
	std::thread writer([&] {
		writerThread = std::this_thread::get_id();
		Signal signal(kBlock);
		for (std::uint32_t b = 0; b < kBlocks; ++b) {
			const std::uint64_t at = std::uint64_t{b} * kBlock;
			sink->tap().write(signal.block(at, kBlock), kBlock, stampAt(at, b == 0));
		}
	});
	writer.join();
	REQUIRE(shared.waitFor([&] { return shared.frames == kBlock * kBlocks; }));
	CHECK(closeAndWait(loop, *sink) == 0);

	const std::scoped_lock lock(shared.mutex);
	REQUIRE(shared.calls.front() == "begin");
	CHECK(shared.calls.back() == "finish");
	REQUIRE(shared.chunks.size() == kBlocks); // nothing wrapped: one chunk per cycle
	checkChunksAgainstSignal(shared.chunks);
	for (std::uint32_t b = 0; b < kBlocks; ++b) {
		INFO("chunk " << b);
		CHECK(shared.chunks[b].position == std::uint64_t{b} * kBlock);
		CHECK(shared.chunks[b].startNs == static_cast<std::int64_t>(b * kBlock) * 1000);
		CHECK(shared.chunks[b].discontinuity == (b == 0));
	}
	const std::thread::id sinkThread = shared.chunks.front().thread;
	for (const Got& chunk : shared.chunks) CHECK(chunk.thread == sinkThread);
	CHECK(sinkThread != writerThread);
	CHECK(sinkThread != std::this_thread::get_id());
}

TEST_CASE("A chunk that wraps the ring's end is consumed in two calls, the second advanced and not a discontinuity",
          "[sink]") {
	PipeWire pipewire;
	TestLoop loop;
	ThreadSinks sinks(loop);
	Shared shared;
	// A 256-frame ring and 100-frame chunks: reads split at its end at varying offsets.
	constexpr std::uint32_t kBlock = 100;
	constexpr std::uint32_t kBlocks = 50;
	auto sink = sinks.make("rec", "writer", 2, 256);
	beginAndWait(loop, *sink, std::make_unique<TestConsumer>(shared));
	Signal signal(kBlock);
	for (std::uint32_t b = 0; b < kBlocks; ++b) {
		const std::uint64_t at = std::uint64_t{b} * kBlock;
		sink->tap().write(signal.block(at, kBlock), kBlock, stampAt(at, true));
		// Each chunk waits for the last to arrive, so the ring never fills and nothing drops.
		REQUIRE(shared.waitFor([&] { return shared.frames == at + kBlock; }));
	}
	CHECK(closeAndWait(loop, *sink) == 0);

	const std::scoped_lock lock(shared.mutex);
	checkChunksAgainstSignal(shared.chunks);
	CHECK(shared.chunks.size() > kBlocks);
	std::size_t split = 0;
	for (const Got& chunk : shared.chunks) {
		const bool cycleStart = chunk.position % kBlock == 0;
		CHECK(chunk.discontinuity == cycleStart); // only a cycle's first call carries its discontinuity
		if (!cycleStart) ++split;
	}
	CHECK(split == shared.chunks.size() - kBlocks);
}

TEST_CASE("A sink's stamp ring and frame ring drop together: a full stamp ring drops whole chunks, a full frame ring "
          "the rest of one, and closed reports every frame dropped",
          "[sink]") {
	PipeWire pipewire;
	TestLoop loop;
	ThreadSinks sinks(loop);
	Shared shared;
	// A 4096-frame ring holds 128 stamps (4096 / 32), so 1-frame chunks run the stamp ring out first; 100-frame chunks
	// run the frame ring out first.
	auto sink = sinks.make("rec", "writer", 2, 4096);
	beginAndWait(loop, *sink, std::make_unique<TestConsumer>(shared, nullptr, true));
	struct Release {
		Gate& gate;
		~Release() { gate.open(); }
	} release{shared.release};
	Signal signal(100);
	std::uint64_t at = 0;
	sink->tap().write(signal.block(at, 1), 1, stampAt(at));
	at += 1;
	REQUIRE(shared.waitFor([&] { return shared.stalled; }));
	// The consumer holds the first stamp, so 127 more fit, then whole chunks drop (the frame ring has room for them).
	for (int i = 0; i < 200; ++i, ++at) sink->tap().write(signal.block(at, 1), 1, stampAt(at));
	const std::uint64_t droppedWhole = 200 - 127;
	shared.release.open();
	REQUIRE(shared.waitFor([&] { return shared.frames == 128; }));
	CHECK(closeAndWait(loop, *sink) == droppedWhole);
	{
		const std::scoped_lock lock(shared.mutex);
		checkChunksAgainstSignal(shared.chunks);
		REQUIRE(shared.chunks.size() == 128);
		CHECK(shared.chunks.back().position == 127); // the chunks dropped were the last ones, whole
	}

	// The frame ring: 100-frame chunks into a 256-frame ring while the consumer stalls on the first.
	Shared second;
	auto small = sinks.make("rec", "writer", 2, 256);
	beginAndWait(loop, *small, std::make_unique<TestConsumer>(second, nullptr, true));
	struct Release2 {
		Gate& gate;
		~Release2() { gate.open(); }
	} release2{second.release};
	std::uint64_t position = 0;
	small->tap().write(signal.block(position, 100), 100, stampAt(position));
	position += 100;
	REQUIRE(second.waitFor([&] { return second.stalled; }));
	for (int i = 0; i < 4; ++i, position += 100)
		small->tap().write(signal.block(position, 100), 100, stampAt(position));
	// 256 frames of room, 100 held: 156 more accepted (100, then 56 of the next, stamped with 56), the rest refused.
	const std::uint64_t refused = 4 * 100 - 156;
	second.release.open();
	REQUIRE(second.waitFor([&] { return second.frames == 256; }));
	CHECK(closeAndWait(loop, *small) == refused);
	const std::scoped_lock lock(second.mutex);
	checkChunksAgainstSignal(second.chunks);
	// The chunk the frame ring cut short keeps its stamp and only the frames it took.
	std::uint64_t total = 0;
	for (const Got& chunk : second.chunks) total += chunk.frames;
	CHECK(total == 256);
	CHECK(second.chunks.back().position + second.chunks.back().frames == 256);
}

TEST_CASE("A sink's close drains what its ring holds, then finishes a consumer that began, then closed runs on the "
          "main loop, and a sink never begun finishes nothing",
          "[sink]") {
	PipeWire pipewire;
	TestLoop loop;
	ThreadSinks sinks(loop);
	Shared shared;
	auto sink = sinks.make("rec", "writer", 2, 1024);
	beginAndWait(loop, *sink, std::make_unique<TestConsumer>(shared));
	Signal signal(64);
	for (std::uint64_t at = 0; at < 512; at += 64) sink->tap().write(signal.block(at, 64), 64, stampAt(at));
	// Closed at once: close drains the rest itself.
	CHECK(closeAndWait(loop, *sink) == 0);
	{
		const std::scoped_lock lock(shared.mutex);
		CHECK(shared.frames == 512);
		CHECK(shared.calls.back() == "finish");
		CHECK(std::ranges::count(shared.calls, std::string("finish")) == 1);
	}

	// Never begun: written frames (none a block would write before begun, but a tap is a tap) are skipped, and close
	// calls neither begin nor finish.
	auto unbegun = sinks.make("rec", "other", 2, 1024);
	unbegun->tap().write(signal.block(0, 64), 64, stampAt(0));
	CHECK(closeAndWait(loop, *unbegun) == 0);
}

TEST_CASE("A lost said inside a sink consumer's begin lands on the main loop before begun", "[sink]") {
	PipeWire pipewire;
	TestLoop loop;
	ThreadSinks sinks(loop);
	Shared shared;
	auto sink = sinks.make("rec", "writer", 2, 1024);
	bool begun = false;
	sink->begin(std::make_unique<TestConsumer>(shared, &loop), [&] {
		const std::scoped_lock lock(shared.mutex);
		shared.calls.emplace_back("begun");
		begun = true;
	});
	REQUIRE(loop.until([&] { return begun; }));
	CHECK(closeAndWait(loop, *sink) == 0);
	const std::scoped_lock lock(shared.mutex);
	CHECK(shared.calls == std::vector<std::string>{"begin", "lost", "begun", "finish"});
}

TEST_CASE("A sink's tap allocates nothing", "[sink]") {
	PipeWire pipewire;
	TestLoop loop;
	ThreadSinks sinks(loop);
	Shared shared;
	constexpr std::uint32_t kBlock = 64;
	constexpr std::uint32_t kBlocks = 50;
	auto sink = sinks.make("rec", "writer", 2, kBlock * kBlocks);
	beginAndWait(loop, *sink, std::make_unique<TestConsumer>(shared));
	std::uint64_t allocations = ~0ULL;
	std::thread writer([&] {
		std::vector<std::vector<float>> planes(2, std::vector<float>(kBlock, 0.25F));
		const std::vector<std::span<const float>> views{planes[0], planes[1]};
		const std::uint64_t before = ear::pipewire::testing::countedAllocations();
		ear::pipewire::testing::countAllocationsOnThisThread(true);
		for (std::uint32_t b = 0; b < kBlocks; ++b)
			sink->tap().write(views, kBlock, stampAt(std::uint64_t{b} * kBlock));
		ear::pipewire::testing::countAllocationsOnThisThread(false);
		allocations = ear::pipewire::testing::countedAllocations() - before;
	});
	writer.join();
	REQUIRE(shared.waitFor([&] { return shared.frames == kBlock * kBlocks; }));
	CHECK(closeAndWait(loop, *sink) == 0);
	CHECK(allocations == 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Through a real block on the in-process engine (node-framework.md 9.6's [U]; node-lifecycle.md 11.6).

namespace {

/// One value per frame of the graph clock, nonzero and exact as a float.
float rampAt(std::uint64_t frame) noexcept { return static_cast<float>((frame & 0xFFFFFFU) + 1U); }

/// Writes rampAt(position + i) to every output (the sink's bank among them).
class PositionRamp final : public AudioCode {
public:
	explicit PositionRamp(ear::pipewire::testing::Waiter& waiter) : waiter_(waiter) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle& cycle) noexcept override {
		for (const std::span<float> out : outputs)
			for (std::uint32_t i = 0; i < frames; ++i) out[i] = rampAt(cycle.position + i);
		if (armed_.exchange(false, std::memory_order_acq_rel))
			firstPosition_.store(cycle.position, std::memory_order_relaxed);
		calls_.fetch_add(1, std::memory_order_release);
		waiter_.wake();
	}
	void release() noexcept override {}
	std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }
	/// The next cycle records its position as the first after a take-up (read once calls() moved past it).
	void arm() { armed_.store(true, std::memory_order_release); }
	std::uint64_t firstPosition() const { return firstPosition_.load(std::memory_order_relaxed); }

private:
	ear::pipewire::testing::Waiter& waiter_;
	std::atomic<std::uint64_t> calls_{0};
	std::atomic<bool> armed_{false};
	std::atomic<std::uint64_t> firstPosition_{0};
};

/// Records every chunk (on the sink's thread), and wakes the waiter.
class RampConsumer final : public SinkConsumer {
public:
	explicit RampConsumer(ear::pipewire::testing::Waiter& waiter) : waiter_(waiter) {}
	void consume(const Chunk& chunk) override {
		{
			const std::scoped_lock lock(mutex);
			chunks.push_back(Got{{chunk.interleaved.begin(), chunk.interleaved.end()},
			                     chunk.frames,
			                     chunk.position,
			                     chunk.startNs,
			                     chunk.discontinuity,
			                     std::this_thread::get_id()});
		}
		waiter_.wake();
	}
	void finish() override {}
	std::mutex mutex;
	std::vector<Got> chunks;

private:
	ear::pipewire::testing::Waiter& waiter_;
};

} // namespace

TEST_CASE("A ramp through a block's sink starts at the first driven sample and ends at the last, across a stop and a "
          "start, under an invoke storm",
          "[sink][blocks]") {
	using ear::pipewire::testing::kFormat;
	auto engine = ear::pipewire::testing::makeEngine();
	ear::pipewire::testing::Waiter waiter(*engine);
	ear::pipewire::testing::NoBell bell;
	FilterPortBlocks blocks(*engine, bell);
	MainLoopExecutor mainLoop(*engine);
	ThreadSinks sinks(mainLoop);
	ear::pipewire::testing::BlockEvents events(waiter);
	const Ports ports{{{Direction::output, "writer", {"left", "right"}, PortBank::Reach::inside}}};
	auto block = blocks.make("rec", ports, kFormat, events);
	auto sink = sinks.make("rec", "writer", 2, 2 * kFormat.rate);
	auto owned = std::make_unique<RampConsumer>(waiter);
	RampConsumer& consumer = *owned;
	bool begun = false;
	sink->begin(std::move(owned), [&] {
		begun = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return begun && events.isReady; }));
	const std::vector<SinkBinding> bindings{SinkBinding{&sink->tap(), 0, 2}};
	PositionRamp ramp(waiter);

	std::vector<std::uint64_t> takens; // start, stop, start, stop
	std::size_t completed = 0;
	const auto record = [&](bool keep) {
		return [&, keep](std::uint64_t taken) {
			if (keep) takens.push_back(taken);
			++completed;
			waiter.wake();
		};
	};
	std::size_t asked = 0;
	// The storm: the same code and taps driven again and again, with fade-outs between (nothing is faded, so each is
	// acknowledged at a cycle's end), while the code keeps running; then a silence.
	const auto storm = [&] {
		for (int i = 0; i < 200; ++i) {
			block->drive(DriveRequest{&ramp, {}, bindings}, record(false));
			++asked;
			if (i % 20 == 0) block->fadeOut([&] { waiter.wake(); });
		}
	};
	// The position of the code's first cycle after each start, as the code itself saw it. Round 0 is the block's first
	// drive with no cycle run before it (its takenAt says 0: Found building L-4b's item 10); round 1 follows a stop.
	std::array<std::uint64_t, 2> firstRun{};
	for (int round = 0; round < 2; ++round) {
		INFO("round " << round);
		ramp.arm();
		block->drive(DriveRequest{&ramp, {}, bindings}, record(true));
		++asked;
		REQUIRE(waiter.until([&] { return completed == asked; }));
		const std::uint64_t calls = ramp.calls();
		REQUIRE(waiter.until([&] { return ramp.calls() >= calls + 4; }));
		firstRun[static_cast<std::size_t>(round)] = ramp.firstPosition();
		storm();
		REQUIRE(waiter.until([&] { return completed == asked; }, 20s));
		const std::uint64_t later = ramp.calls();
		REQUIRE(waiter.until([&] { return ramp.calls() >= later + 4; }));
		block->drive(DriveRequest{nullptr, {}}, record(true));
		++asked;
		REQUIRE(waiter.until([&] { return completed == asked; }));
		// Stopped for a while: the block runs silent, the sink unbound.
		std::this_thread::sleep_for(50ms);
	}
	bool closed = false;
	std::uint64_t dropped = ~0ULL;
	sink->close([&](std::uint64_t d) {
		dropped = d;
		closed = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return closed; }));
	CHECK(dropped == 0);
	block.reset();

	REQUIRE(takens.size() == 4);
	const std::scoped_lock lock(consumer.mutex);
	REQUIRE_FALSE(consumer.chunks.empty());
	// Every frame the sink got is the ramp at its position, contiguous within each run, and each run is exactly
	// [start's position, stop's position).
	std::vector<std::pair<std::uint64_t, std::uint64_t>> runs; // [first, end) of each contiguous stretch
	for (const Got& chunk : consumer.chunks) {
		INFO("chunk at " << chunk.position);
		REQUIRE(chunk.interleaved.size() == static_cast<std::size_t>(chunk.frames) * 2);
		for (std::uint32_t f = 0; f < chunk.frames; ++f) {
			REQUIRE(chunk.interleaved[f * 2] == rampAt(chunk.position + f));
			REQUIRE(chunk.interleaved[f * 2 + 1] == rampAt(chunk.position + f));
		}
		if (!runs.empty() && runs.back().second == chunk.position) runs.back().second += chunk.frames;
		else runs.emplace_back(chunk.position, chunk.position + chunk.frames);
	}
	INFO("starts and stops taken at " << takens[0] << ", " << takens[1] << ", " << takens[2] << ", " << takens[3]
	                                  << "; the code's first cycles at " << firstRun[0] << ", " << firstRun[1]);
	if (runs.size() != 2) {
		// The clock jumped (an xrun) inside a run: say so; the bounds below still hold.
		WARN("the sink's frames came in " << runs.size() << " stretches: the graph clock jumped while it ran");
	}
	// None before the code's first cycle after a start, none after a stop's position.
	for (const auto& [first, end] : runs) {
		const bool inFirst = first >= firstRun[0] && end <= takens[1];
		const bool inSecond = first >= firstRun[1] && end <= takens[3];
		CHECK((inFirst || inSecond));
	}
	for (std::size_t round = 0; round < 2; ++round) {
		INFO("round " << round);
		const std::uint64_t stop = takens[2 * round + 1];
		std::optional<std::uint64_t> first;
		std::uint64_t end = 0;
		for (const auto& [from, to] : runs)
			if (from >= firstRun[round] && to <= stop) {
				if (!first) first = from;
				end = std::max(end, to);
			}
		REQUIRE(first.has_value());
		// The sink's first frame is the code's first cycle after the start, always: the tap is bound in the same
		// invoke as the code, so no cycle of it is lost or added.
		CHECK(*first == firstRun[round]);
		// Its last frame is the last the code wrote before its silence took effect, always.
		CHECK(end == stop);
		// The start's reported position is that first cycle's, unless the block had run no cycle yet (round 0) or the
		// clock jumped right at the take-up (an xrun under load): the only excusable comparison.
		const std::uint64_t start = takens[2 * round];
		if (round == 1 && start != firstRun[round])
			WARN("an xrun at the take-up: the start taken at " << start << ", its code's first cycle at "
			                                                   << firstRun[round]);
		if (round == 1) CHECK(start <= firstRun[round]);
	}
}
