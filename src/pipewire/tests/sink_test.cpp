// The interim sinks (docs/engine.md#sinks): the real ThreadSinks, first on its own thread with a test's main loop
// (order, stamps, a chunk wrapping the ring's end, the two rings dropping together, close draining then finishing,
// begin before the first consume and `begun` behind a lost said inside begin, no allocation on the tap, and the same
// cycles through the type harness's sink handed alike, call by call), then through a
// real block on the in-process engine (a ramp recorded through the sink starts at the first driven sample and ends at
// the last, across a stop and a start, under an invoke storm; each chunk stamped with the capture latency the block
// tells on the sink's bank; the dropped count read while the block writes). Every wait is event-gated: a test waits for
// the event it needs; the deadline only fails the test.
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
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <pipewire/pipewire.h>

#include "allocation_count.hpp"
#include "catch2/catch.hpp"
#include "ear/pipewire/handoffs.hpp"
#include "ear/pipewire/links.hpp"
#include "ear/pipewire/live_graph.hpp"
#include "ear/pipewire/main_loop_executor.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/testing/drive_now.hpp"
#include "ear/testing/type_harness.hpp"
#include "engine_harness.hpp"
#include "cycle_judge.hpp"
#include "inducers.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace std::chrono_literals;

constexpr auto kDeadline = 5s;
/// The rate the sinks run at off the engine: a frame lasts exactly 20,000 ns.
constexpr std::uint32_t kTestRate = 50000;
constexpr std::int64_t kFrameNs = 1'000'000'000 / kTestRate;

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
	std::int64_t startRawNs;
	std::uint32_t upstreamFrames;
	CycleCauses causes;
	std::uint64_t lostFrames;
	std::thread::id thread;
	bool discontinuity() const noexcept { return !causes.empty(); }
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
			                             chunk.startRawNs,
			                             chunk.upstreamFrames,
			                             chunk.causes,
			                             chunk.lostFrames,
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

/// A stamp at `position`: both clocks at position * 1000 ns, and an upstream latency the block would have told, a
/// value per position so a chunk shows it came with its own stamp; with `caughtUp`, a catch-up of a block's frames
/// before it (causes and frames lost), which a split chunk's first call alone carries.
ChunkStamp stampAt(std::uint64_t position, bool caughtUp = false) {
	return ChunkStamp{.position = position,
	                  .startNs = static_cast<std::int64_t>(position) * 1000,
	                  .startRawNs = static_cast<std::int64_t>(position) * 1000,
	                  .causes = caughtUp ? CycleCauses(CycleCause::catchUp) : CycleCauses{},
	                  .lostFrames = caughtUp ? 100U : 0U,
	                  .upstreamFrames = static_cast<std::uint32_t>(position % 997)};
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
	auto sink = sinks.make("a-node-with-a-long-name", "writer", 2, kBlock * kBlocks, kTestRate);
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
		CHECK(shared.chunks[b].upstreamFrames == stampAt(std::uint64_t{b} * kBlock).upstreamFrames);
		CHECK(shared.chunks[b].discontinuity() == (b == 0));
	}
	const std::thread::id sinkThread = shared.chunks.front().thread;
	for (const Got& chunk : shared.chunks) CHECK(chunk.thread == sinkThread);
	CHECK(sinkThread != writerThread);
	CHECK(sinkThread != std::this_thread::get_id());
}

TEST_CASE("A chunk that wraps the ring's end is consumed in two calls, the second advanced in position and start "
          "times and with no causes or frames lost, so each call dates its own first frame",
          "[sink]") {
	PipeWire pipewire;
	TestLoop loop;
	ThreadSinks sinks(loop);
	Shared shared;
	// A 256-frame ring and 100-frame chunks: reads split at its end at varying offsets.
	constexpr std::uint32_t kBlock = 100;
	constexpr std::uint32_t kBlocks = 50;
	auto sink = sinks.make("rec", "writer", 2, 256, kTestRate);
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
		// Only a cycle's first call carries its causes and the frames lost before it.
		CHECK(chunk.causes == (cycleStart ? CycleCauses(CycleCause::catchUp) : CycleCauses{}));
		CHECK(chunk.lostFrames == (cycleStart ? 100U : 0U));
		const ChunkStamp cycle = stampAt(chunk.position - chunk.position % kBlock);
		CHECK(chunk.upstreamFrames == cycle.upstreamFrames); // both calls
		// Each call's start is its first frame's: the cycle's start plus the frames before it in the chunk. So the
		// instant its first frame entered the graph (Chunk::upstreamFrames' rule) is the cycle's first frame's plus
		// those frames too.
		const auto offset = static_cast<std::int64_t>(chunk.position % kBlock) * kFrameNs;
		CHECK(chunk.startNs == cycle.startNs + offset);
		CHECK(chunk.startRawNs == cycle.startRawNs + offset);
		const std::int64_t entered = chunk.startRawNs - static_cast<std::int64_t>(chunk.upstreamFrames) * kFrameNs;
		CHECK(entered == cycle.startRawNs - static_cast<std::int64_t>(cycle.upstreamFrames) * kFrameNs + offset);
		if (!cycleStart) ++split;
	}
	CHECK(split == shared.chunks.size() - kBlocks);
}

TEST_CASE("A sink's stamp ring and frame ring drop together: a full stamp ring drops whole chunks, a full frame ring "
          "the rest of one, droppedFrames counts them as they drop, and closed reports every frame dropped",
          "[sink]") {
	PipeWire pipewire;
	TestLoop loop;
	ThreadSinks sinks(loop);
	Shared shared;
	// A 4096-frame ring holds 128 stamps (4096 / 32), so 1-frame chunks run the stamp ring out first; 100-frame chunks
	// run the frame ring out first.
	auto sink = sinks.make("rec", "writer", 2, 4096, kTestRate);
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
	CHECK(sink->droppedFrames() == droppedWhole); // readable while the sink's thread runs, before any close
	shared.release.open();
	REQUIRE(shared.waitFor([&] { return shared.frames == 128; }));
	CHECK(closeAndWait(loop, *sink) == droppedWhole);
	CHECK(sink->droppedFrames() == droppedWhole);
	{
		const std::scoped_lock lock(shared.mutex);
		checkChunksAgainstSignal(shared.chunks);
		REQUIRE(shared.chunks.size() == 128);
		CHECK(shared.chunks.back().position == 127); // the chunks dropped were the last ones, whole
	}

	// The frame ring: 100-frame chunks into a 256-frame ring while the consumer stalls on the first.
	Shared second;
	auto small = sinks.make("rec", "writer", 2, 256, kTestRate);
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
	CHECK(small->droppedFrames() == refused);
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
	auto sink = sinks.make("rec", "writer", 2, 1024, kTestRate);
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
	auto unbegun = sinks.make("rec", "other", 2, 1024, kTestRate);
	unbegun->tap().write(signal.block(0, 64), 64, stampAt(0));
	CHECK(closeAndWait(loop, *unbegun) == 0);
}

TEST_CASE("A lost said inside a sink consumer's begin lands on the main loop before begun", "[sink]") {
	PipeWire pipewire;
	TestLoop loop;
	ThreadSinks sinks(loop);
	Shared shared;
	auto sink = sinks.make("rec", "writer", 2, 1024, kTestRate);
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
	auto sink = sinks.make("rec", "writer", 2, kBlock * kBlocks, kTestRate);
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
// The sink's conformance (docs/writing-a-node-type.md#testing-a-type): the same cycles through the real ThreadSink and
// through the type harness's sink hand a consumer the same calls, split at the ring's end in the same places.

// Named, never unnamed: a type spec is a template key (type_spec.hpp says why).
namespace sink_conformance {

using namespace ear;

inline constexpr auto record = bank::input<"record">;
inline constexpr auto writer = bank::insideOutput<"writer">;
inline constexpr TypeSpec spec{.settings = SettingsTable<>{"recorder"}, .banks = Banks<record, writer>{}};
using Checked = CheckedOf<spec>;

/// Where the type's consumer records: the test sets it before the harness configures the node.
Shared* recorded = nullptr;
/// What the type declares on its path into the sink, so the stamps carry an upstream latency that is not zero.
inline constexpr Latency kDeclared{30, 40};

/// file's shape: a sink on `writer` fed by the pass-through from `record`, its consumer the test's.
struct Recorder {
	static Members<spec> ports(const Checked&) { return {record(members(2)), writer(members(2))}; }
	Configured configure(CreateOf<spec>& cx, const Checked&) {
		cx.sink(writer, SinkShape{.frames = 4096}).consumedBy(std::make_unique<TestConsumer>(*recorded));
		cx.use(passThrough({bankPair(record, writer)}));
		cx.declareLatency(record, writer, kDeclared);
		return Configured::applied();
	}
};

} // namespace sink_conformance

TEST_CASE("A chunk split at the ring's end reaches its consumer the same through ThreadSink and through the type "
          "harness's sink, with the same calls, frames, positions, start times, latency and discontinuities",
          "[sink][conformance]") {
	namespace conform = sink_conformance;
	// A 256-frame ring and 100-frame cycles, as the wrap test above: chunks split at the ring's end at varying offsets.
	constexpr std::uint32_t kRing = 256;
	constexpr std::uint32_t kBlock = 100;
	constexpr std::uint32_t kBlocks = 12;
	const FrameClock clock(kTestRate);

	Shared throughHarness;
	conform::recorded = &throughHarness;
	{
		ear::testing::TypeHarness<conform::spec, conform::Recorder> harness(
			{.format = {.rate = kTestRate, .maxBlock = kBlock}, .node = "rec", .sinkRingFrames = kRing});
		REQUIRE_FALSE(harness.parse({}));
		harness.add();
		harness.blockReady();
		REQUIRE(harness.phase() == "running");
		for (std::uint32_t b = 0; b < kBlocks; ++b) {
			for (std::uint32_t c = 0; c < 2; ++c) {
				const std::span<float> in = harness.input(conform::record, c);
				for (std::uint32_t f = 0; f < kBlock; ++f)
					in[f] = static_cast<float>((std::uint64_t{b} * kBlock + f) * 2 + c);
			}
			harness.process(kBlock);
		}
		harness.remove();
		CHECK(harness.droppedFrames() == 0);
	}
	conform::recorded = nullptr;

	// The engine's sink, written as the real block writes it: each cycle stamped with its position, both clocks at the
	// graph's rate from zero (the harness's clock), the block's first cycle (start and drive) the first, and the most of
	// the latency declared on the sink's bank, which a block with no peers tells there.
	PipeWire pipewire;
	TestLoop loop;
	ThreadSinks sinks(loop);
	Shared throughEngine;
	auto sink = sinks.make("rec", "writer", 2, kRing, kTestRate);
	beginAndWait(loop, *sink, std::make_unique<TestConsumer>(throughEngine));
	Signal signal(kBlock);
	for (std::uint32_t b = 0; b < kBlocks; ++b) {
		const std::uint64_t at = std::uint64_t{b} * kBlock;
		const std::int64_t startNs = clock.ns(static_cast<std::int64_t>(at));
		sink->tap().write(signal.block(at, kBlock), kBlock,
		                  ChunkStamp{.position = at,
		                             .startNs = startNs,
		                             .startRawNs = startNs,
		                             .causes = b == 0 ? CycleCause::start | CycleCause::drive : CycleCauses{},
		                             .lostFrames = 0,
		                             .upstreamFrames = conform::kDeclared.maxFrames});
		// Each chunk waits for the last to arrive, so the ring never fills and nothing drops.
		REQUIRE(throughEngine.waitFor([&] { return throughEngine.frames == at + kBlock; }));
	}
	CHECK(closeAndWait(loop, *sink) == 0);

	const std::scoped_lock lock(throughEngine.mutex, throughHarness.mutex);
	checkChunksAgainstSignal(throughEngine.chunks);
	REQUIRE(throughEngine.chunks.size() > kBlocks); // some cycles were split
	REQUIRE(throughHarness.chunks.size() == throughEngine.chunks.size());
	for (std::size_t i = 0; i < throughEngine.chunks.size(); ++i) {
		const Got& engine = throughEngine.chunks[i];
		const Got& harness = throughHarness.chunks[i];
		INFO("call " << i << " at " << engine.position);
		CHECK(harness.frames == engine.frames);
		CHECK(harness.position == engine.position);
		CHECK(harness.startNs == engine.startNs);
		CHECK(harness.startRawNs == engine.startRawNs);
		CHECK(harness.upstreamFrames == engine.upstreamFrames);
		CHECK(harness.causes == engine.causes);
		CHECK(harness.lostFrames == engine.lostFrames);
		CHECK(harness.interleaved == engine.interleaved);
	}
	CHECK(throughHarness.calls == throughEngine.calls); // begin, each consume, finish
}

// ---------------------------------------------------------------------------------------------------------------------
// Through a real block on the in-process engine (docs/engine.md#sinks).

namespace {

/// One value per frame of the graph clock, nonzero and exact as a float.
float rampAt(std::uint64_t frame) noexcept { return static_cast<float>((frame & 0xFFFFFFU) + 1U); }

/// Writes rampAt(position + i) to every output (the sink's bank among them), and records every cycle it runs in its
/// ledger: the frames the sink must get are exactly those of these cycles.
class PositionRamp final : public AudioCode {
public:
	explicit PositionRamp(ear::pipewire::testing::Waiter& waiter, std::size_t capacity = 0)
		: waiter_(waiter), ledger_(capacity) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle& cycle) noexcept override {
		for (const std::span<float> out : outputs)
			for (std::uint32_t i = 0; i < frames; ++i) out[i] = rampAt(cycle.position + i);
		ledger_.record(cycle, frames); // a full ledger takes no more and says so: overflowed()
		calls_.fetch_add(1, std::memory_order_release);
		waiter_.wake();
	}
	void release() noexcept override {}
	std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }
	/// The cycles it ran, up to the capacity it was made with.
	const ear::test::CycleLedger& ledger() const { return ledger_; }

private:
	ear::pipewire::testing::Waiter& waiter_;
	ear::test::CycleLedger ledger_;
	std::atomic<std::uint64_t> calls_{0};
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
			                     chunk.startRawNs,
			                     chunk.upstreamFrames,
			                     chunk.causes,
			                     chunk.lostFrames,
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
	// The take-ups judged exactly, each a start after a stop whose first cycle has no cause but its drive: nothing was
	// lost before it. A host that stalls the data thread across a take-up makes another round, never a pass or a
	// failure. Every cycle the code runs goes through the one judge (docs/testing.md#the-cycle-judge): judged where
	// nothing was lost, excused where the driver cut what the block missed, a drive's first cycle expected; anything
	// else fails. The block counting the graph's cycles feeds the block under test, and makes the judge's stalls in
	// the first round while the code runs, so the excuse is taken, and proven, on every run.
	constexpr std::size_t kJudgedStarts = 2;
	constexpr std::size_t kMostRounds = 40; // only fails the test
	/// Graph cycles a stop lasts, counted by a block of its own.
	constexpr std::uint64_t kStoppedCycles = 8;
	auto engine = ear::pipewire::testing::makeEngine();
	ear::pipewire::testing::Waiter waiter(*engine);
	ear::pipewire::testing::NoBell bell;
	FilterPortBlocks blocks(*engine, bell);
	MainLoopExecutor mainLoop(*engine);
	ThreadSinks sinks(mainLoop);
	ear::pipewire::testing::BlockEvents events(waiter), clockEvents(waiter);
	// The block under test has an input, fed by the block that counts the graph's cycles: that one runs first in each
	// cycle, so a stall there cuts the cycle before the block under test ran it, and the code's own facts state the cut
	// (docs/testing.md#the-cycle-judge). A stall in the block under test would cut only what follows it.
	const Ports ports{
		{{Direction::input, "in", {"1"}}, {Direction::output, "writer", {"left", "right"}, PortBank::Reach::inside}}};
	auto block = blocks.make("rec", ports, kFormat, events);
	auto clock = blocks.make("clock", Ports{{{Direction::output, "out", {"1"}}}}, kFormat, clockEvents);
	auto sink = sinks.make("rec", "writer", 2, 2 * kFormat.rate, kFormat.rate);
	auto owned = std::make_unique<RampConsumer>(waiter);
	RampConsumer& consumer = *owned;
	bool begun = false;
	sink->begin(std::move(owned), [&] {
		begun = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return begun && events.isReady && clockEvents.isReady; }));
	ear::test::ArmedStall counting(kFormat.rate, [&] { waiter.wake(); });
	REQUIRE(ear::testing::driveNow(*clock, &counting));
	LiveGraph live(*engine);
	Links links(*engine);
	std::vector<LivePort> seen;
	struct PortSeen final : LivePortListener {
		explicit PortSeen(ear::pipewire::testing::Waiter& waiter, std::vector<LivePort>& seen)
			: waiter(waiter), seen(seen) {}
		void portAppeared(const LivePort& port) override {
			seen.push_back(port);
			waiter.wake();
		}
		void portGone(EnginePortId) override {}
		ear::pipewire::testing::Waiter& waiter;
		std::vector<LivePort>& seen;
	} portLog(waiter, seen);
	struct Linked final : LinkListener {
		explicit Linked(ear::pipewire::testing::Waiter& waiter) : waiter(waiter) {}
		void linkActive(LinkId) override {
			active = true;
			waiter.wake();
		}
		void linkFailed(LinkId, std::string) override {
			failed = true;
			waiter.wake();
		}
		void linkGone(LinkId) override {}
		ear::pipewire::testing::Waiter& waiter;
		bool active = false, failed = false;
	} linkLog(waiter);
	live.subscribe(portLog);
	links.subscribe(linkLog);
	const auto port = [&](const char* node, const char* name) -> std::optional<EnginePortId> {
		for (const LivePort& live : seen)
			if (live.port.node == node && live.port.port == name) return live.id;
		return std::nullopt;
	};
	REQUIRE(waiter.until([&] { return port("clock", "out.1") && port("rec", "in.1"); }));
	links.createLink(1, *port("clock", "out.1"), *port("rec", "in.1"));
	REQUIRE(waiter.until([&] { return linkLog.active || linkLog.failed; }));
	REQUIRE_FALSE(linkLog.failed);
	const std::vector<SinkBinding> bindings{SinkBinding{&sink->tap(), 0, 2}};
	PositionRamp ramp(waiter, std::size_t{1} << 16);
	// Each drive of the code states `drive` on the cycle it is taken at; drives the data loop takes before the same
	// cycle share it. The plan bounds them by the most the test asks; the report must hold exactly as many as were
	// taken at distinct positions (below).
	ear::test::CycleJudge judge(
		ear::test::JudgePlan{.judged = 32,
	                         .induce = ear::test::StallPlan::every(1, 0, 4, ear::test::StallLength::cycles(1.5)),
	                         .expected = {.drives = static_cast<std::uint32_t>(kMostRounds * 202)}},
		FrameClock(kFormat.rate), &engine->cuts());
	std::size_t fed = 0;
	const auto feed = [&] {
		const ear::test::CycleLedger& ledger = ramp.ledger();
		for (; fed < ledger.count(); ++fed) judge.at(ledger[fed].facts, ledger[fed].frames);
	};

	std::vector<std::uint64_t> takens; // per round: its start's position, then its stop's
	std::set<std::uint64_t> codeTaken; // where each drive of the code was taken: one `drive` each
	std::size_t completed = 0;
	const auto record = [&](bool keep, bool code = true) {
		return [&, keep, code](std::uint64_t taken) {
			if (keep) takens.push_back(taken);
			if (code) codeTaken.insert(taken);
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
	// The ledger index of each round's first cycle: the code runs in no other block and not while stopped, so its next
	// cycle after a start is that one. Round 0 is the block's first drive (its takenAt says where the block was, not
	// judged); each later one follows a stop.
	std::vector<std::size_t> firstCycle;
	std::size_t judged = 0;
	for (std::size_t round = 0; round < 2 || judged < kJudgedStarts || !judge.done(); ++round) {
		INFO("round " << round << ", " << judged << " take-ups judged so far");
		REQUIRE(round < kMostRounds);
		firstCycle.push_back(ramp.ledger().count());
		block->drive(DriveRequest{&ramp, {}, bindings}, record(true));
		++asked;
		REQUIRE(waiter.until([&] { return completed == asked; }));
		REQUIRE(waiter.until([&] { return ramp.calls() >= firstCycle.back() + 4; }));
		// The judge's stalls, one at a time while the code runs: each cuts its cycle before the block ran it.
		while (judge.stalls().made() < judge.stalls().plan().size()) {
			const std::size_t made = judge.stalls().made();
			counting.arm(judge.stalls());
			REQUIRE(waiter.until([&] { return judge.stalls().made() > made; }));
			const std::uint64_t calls = ramp.calls();
			REQUIRE(waiter.until([&] { return ramp.calls() >= calls + 2; }));
		}
		storm();
		REQUIRE(waiter.until([&] { return completed == asked; }, 20s));
		const std::uint64_t later = ramp.calls();
		REQUIRE(waiter.until([&] { return ramp.calls() >= later + 4; }));
		block->drive(DriveRequest{nullptr, {}}, record(true, false));
		++asked;
		REQUIRE(waiter.until([&] { return completed == asked; }));
		// Stopped for kStoppedCycles of the graph's: the block runs silent, the sink unbound.
		const std::uint64_t counted = counting.calls();
		REQUIRE(waiter.until([&] { return counting.calls() >= counted + kStoppedCycles; }));
		// The take-up's first cycle states what was lost before it (docs/engine.md#graphcycle): its drive alone means
		// nothing.
		const ear::test::CycleLedger& ledger = ramp.ledger();
		REQUIRE_FALSE(ledger.overflowed());
		if (round > 0 && ledger[firstCycle[round]].facts.causes == CycleCauses(CycleCause::drive)) ++judged;
		feed();
		INFO(judge.report());
		REQUIRE(judge.report().failed == 0);
	}
	REQUIRE(ear::testing::driveNow(*clock, nullptr));
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
	clock.reset();

	const std::size_t rounds = firstCycle.size();
	REQUIRE(takens.size() == 2 * rounds);
	REQUIRE_FALSE(ramp.ledger().overflowed());
	const std::span<const ear::test::LedgerCycle> cycles = ramp.ledger().cycles();
	const std::scoped_lock lock(consumer.mutex);
	REQUIRE_FALSE(consumer.chunks.empty());
	// Every frame the sink got is the ramp at its position, and its contiguous stretches are exactly those of the
	// cycles the code ran: no cycle of the code's is lost or added across a stop, a start, a storm of drives, or a
	// jump of the clock the host made (which splits both alike).
	using Stretches = std::vector<std::pair<std::uint64_t, std::uint64_t>>; // [first, end) of each
	const auto extend = [](Stretches& stretches, std::uint64_t position, std::uint64_t frames) {
		if (!stretches.empty() && stretches.back().second == position) stretches.back().second += frames;
		else stretches.emplace_back(position, position + frames);
	};
	Stretches heard, ran;
	for (const Got& chunk : consumer.chunks) {
		INFO("chunk at " << chunk.position);
		REQUIRE(chunk.interleaved.size() == static_cast<std::size_t>(chunk.frames) * 2);
		for (std::uint32_t f = 0; f < chunk.frames; ++f) {
			REQUIRE(chunk.interleaved[f * 2] == rampAt(chunk.position + f));
			REQUIRE(chunk.interleaved[f * 2 + 1] == rampAt(chunk.position + f));
		}
		extend(heard, chunk.position, chunk.frames);
	}
	for (const ear::test::LedgerCycle& cycle : cycles) extend(ran, cycle.position, cycle.frames);
	INFO(rounds << " rounds, " << judged << " take-ups judged; the sink heard " << heard.size()
	            << " stretches, the code ran " << ran.size());
	CHECK(heard == ran);
	feed();
	INFO(judge.report());
	CHECK(judge.report().ok());
	CHECK(judge.report().expected == codeTaken.size()); // exactly the drives made, each once
	for (std::size_t round = 0; round < rounds; ++round) {
		INFO("round " << round);
		const std::size_t first = firstCycle[round];
		const std::size_t last = (round + 1 < rounds ? firstCycle[round + 1] : cycles.size()) - 1;
		REQUIRE(first <= last);
		const std::uint64_t start = takens[2 * round];
		const std::uint64_t stop = takens[2 * round + 1];
		INFO("start taken at " << start << ", stop at " << stop << "; the code's first cycle at "
		                       << cycles[first].position << ", its last ending at "
		                       << cycles[last].position + cycles[last].frames);
		// The stop's position is where the code's last cycle ended, always: the tap is unbound in the same invoke.
		CHECK(cycles[last].position + cycles[last].frames == stop);
		if (round == 0) continue;
		// The start's position is its first cycle's when nothing was lost before it; a cut there (a stall the host
		// made) only puts the first cycle later.
		CHECK(start <= cycles[first].position);
		if (cycles[first].facts.causes == CycleCauses(CycleCause::drive)) CHECK(start == cycles[first].position);
	}
}

TEST_CASE("A sink's chunks carry the capture latency its block tells on the sink's bank, a companion's beyond its "
          "paired input included, and follow each new plan",
          "[sink][blocks][latency]") {
	using ear::pipewire::testing::kFormat;
	auto engine = ear::pipewire::testing::makeEngine();
	ear::pipewire::testing::Waiter waiter(*engine);
	ear::pipewire::testing::NoBell bell;
	FilterPortBlocks blocks(*engine, bell);
	MainLoopExecutor mainLoop(*engine);
	ThreadSinks sinks(mainLoop);
	ear::pipewire::testing::BlockEvents events(waiter);
	// No links: no peer reports anything on `record` or `from_rtp`, so the bank tells what the plan gives it.
	const Ports ports{{{Direction::input, "record", {"1"}},
	                   {Direction::input, "from_rtp", {"1"}, PortBank::Reach::inside},
	                   {Direction::output, "writer", {"1"}, PortBank::Reach::inside}}};
	auto block = blocks.make("rec", ports, kFormat, events);
	auto sink = sinks.make("rec", "writer", 1, 2 * kFormat.rate, kFormat.rate);
	auto owned = std::make_unique<RampConsumer>(waiter);
	RampConsumer& consumer = *owned;
	bool begun = false;
	sink->begin(std::move(owned), [&] {
		begun = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return begun && events.isReady; }));
	const std::vector<SinkBinding> bindings{SinkBinding{&sink->tap(), 0, 1}};
	PositionRamp ramp(waiter);
	const auto latest = [&]() -> std::optional<std::uint32_t> {
		const std::scoped_lock lock(consumer.mutex);
		if (consumer.chunks.empty()) return std::nullopt;
		return consumer.chunks.back().upstreamFrames;
	};

	// The configuration's own latency on the sink's bank.
	block->carryLatency(
		LatencyPlan{.pairs = {{"record", "writer"}}, .onPair = {{{"record", "writer"}, Delay{{7, 9}}}}});
	bool driven = false;
	block->drive(DriveRequest{&ramp, {}, bindings}, [&](std::uint64_t) {
		driven = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return driven; }));
	CHECK(waiter.until([&] { return latest() == 9U; }));
	// A receiver's 480 to 960 beyond `from_rtp`, paired with `writer` too: the most upstream is 960 + 9, though nothing
	// reaches `record`.
	block->carryLatency(
		LatencyPlan{.pairs = {{"record", "writer"}, {"from_rtp", "writer"}},
	                .onPair = {{{"record", "writer"}, Delay{{7, 9}}}, {{"from_rtp", "writer"}, Delay{{7, 9}}}},
	                .beyond = {{"from_rtp", Delay{{480, 960}}}}});
	CHECK(waiter.until([&] { return latest() == 969U; }));
	// An empty plan: nothing upstream at all.
	block->carryLatency({});
	CHECK(waiter.until([&] { return latest() == 0U; }));

	REQUIRE(ear::testing::driveNow(*block, nullptr));
	bool closed = false;
	sink->close([&](std::uint64_t) {
		closed = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return closed; }));
	block.reset();
	// Every chunk carried one of the plans' values, in the plans' order.
	const std::scoped_lock lock(consumer.mutex);
	const std::vector<std::uint32_t> order{9, 969, 0};
	std::size_t at = 0;
	for (const Got& chunk : consumer.chunks) {
		INFO("chunk at " << chunk.position << " with " << chunk.upstreamFrames);
		while (at < order.size() && order[at] != chunk.upstreamFrames) ++at;
		REQUIRE(at < order.size());
	}
}

TEST_CASE("A sink's dropped count is readable while its block writes it, and closed reports what it last read",
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
	// A ring of 1024 frames and a consumer that stalls in its first consume: the ring fills within a few cycles.
	auto sink = sinks.make("rec", "writer", 2, 1024, kFormat.rate);
	Shared shared;
	struct Release {
		Gate& gate;
		~Release() { gate.open(); }
	} release{shared.release};
	bool begun = false;
	sink->begin(std::make_unique<TestConsumer>(shared, nullptr, true), [&] {
		begun = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return begun && events.isReady; }));
	const std::vector<SinkBinding> bindings{SinkBinding{&sink->tap(), 0, 2}};
	PositionRamp ramp(waiter);
	bool driven = false;
	block->drive(DriveRequest{&ramp, {}, bindings}, [&](std::uint64_t) {
		driven = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return driven; }));
	// The ramp wakes the waiter every cycle, so the count is read again each cycle while the block writes the tap.
	REQUIRE(waiter.until([&] { return sink->droppedFrames() >= 4 * kFormat.maxBlock; }));
	const std::uint64_t seen = sink->droppedFrames();
	REQUIRE(ear::testing::driveNow(*block, nullptr));
	const std::uint64_t silenced = sink->droppedFrames(); // nothing writes the tap any more
	CHECK(silenced >= seen);
	shared.release.open();
	std::uint64_t dropped = 0;
	bool closed = false;
	sink->close([&](std::uint64_t d) {
		dropped = d;
		closed = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return closed; }));
	block.reset();
	CHECK(dropped == silenced);
	CHECK(sink->droppedFrames() == silenced);
}
