// The sources (docs/writing-a-node-type.md#sources, docs/engine.md#sources): the real EventSources, first on real
// threads with no engine (a write waits for room and play wakes it, an idle waits its patience, a close wakes a waiting
// write at once and a later one sleeps its whole patience, the played wake rung once until answered, play allocating
// nothing), then served by a real block on the in-process engine: played into its inside input bank before the code in
// the same cycle and unbound by a silence, the latency the block tells on its bank dating what it played, nothing
// allocated on the data thread, and spike S5's part A on the real block (docs/engine.md#the-one-data-loop-and-invokes):
// its played count keeps to the clock across cycles cut short by an upstream probe that sleeps past the quantum. Every
// wait is event-gated: a test waits for the event it needs; the deadline only fails the test.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <pipewire/pipewire.h>

#include "allocation_count.hpp"
#include "catch2/catch.hpp"
#include "ear/pipewire/links.hpp"
#include "ear/pipewire/live_graph.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/sources.hpp"
#include "ear/testing/drive_now.hpp"
#include "engine_harness.hpp"
#include "probes.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace ear::pipewire::testing;
using namespace std::chrono_literals;

using Clock = std::chrono::steady_clock;

/// One value per frame of a stream, nonzero and exact as a float.
float rampAt(std::uint64_t frame) noexcept { return static_cast<float>((frame & 0xFFFFFFU) + 1U); }

/// Frames [from, from + count) of the ramp, one channel.
std::vector<float> ramp(std::uint64_t from, std::uint32_t count) {
	std::vector<float> out(count);
	for (std::uint32_t i = 0; i < count; ++i) out[i] = rampAt(from + i);
	return out;
}

/// One cycle of `frames` at `position`, as the block would hand it.
SourceCycle cycleAt(std::uint64_t position, std::uint32_t frames, Latency downstream = {}) {
	return SourceCycle{.position = position,
	                   .frames = frames,
	                   .startRawNs = static_cast<std::int64_t>(position) * 1000,
	                   .discontinuity = false,
	                   .downstream = downstream};
}

/// A one-channel bank of `frames` frames for play().
struct Bank {
	explicit Bank(std::uint32_t frames) : data(frames), spans{std::span<float>(data)} {}
	std::span<const std::span<float>> view() { return spans; }
	std::vector<float> data;
	std::vector<std::span<float>> spans;
};

/// Whether `fd` is readable now.
bool readable(int fd) {
	pollfd ready{fd, POLLIN, 0};
	return ::poll(&ready, 1, 0) == 1 && (ready.revents & POLLIN) != 0;
}

std::int64_t threadCpuNs() {
	timespec now{};
	::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
	return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

} // namespace

TEST_CASE("A source's write waits for room and is woken by play, and takes what fits", "[source]") {
	EventSources sources;
	auto source = sources.make("rx", "from_writer", 1, 64, kRate);
	SourceWriter& writer = source->writer();
	CHECK(writer.capacityFrames() == 64);
	REQUIRE(writer.write(ramp(0, 64), 64, 0ms) == 64);
	CHECK(writer.write(ramp(64, 16), 16, 0ms) == 0); // full, no patience: nothing
	std::atomic<bool> returned{false};
	std::uint32_t accepted = 0;
	Clock::duration took{};
	std::thread writing([&] {
		const auto began = Clock::now();
		accepted = writer.write(ramp(64, 16), 16, 10s);
		took = Clock::now() - began;
		returned.store(true);
	});
	std::this_thread::sleep_for(50ms); // the writer waits meanwhile
	CHECK_FALSE(returned.load());
	Bank bank(16);
	source->tap().play(bank.view(), cycleAt(0, 16));
	writing.join();
	CHECK(accepted == 16);
	CHECK(took >= 40ms);
	CHECK(took < 5s); // woken by play, not by its patience
}

TEST_CASE("A source's idle waits its patience and makes an empty ring no shortfall", "[source]") {
	EventSources sources;
	auto source = sources.make("rx", "from_writer", 1, 64, kRate);
	SourceWriter& writer = source->writer();
	REQUIRE(writer.write(ramp(0, 16), 16, 0ms) == 16);
	Bank bank(16);
	source->tap().play(bank.view(), cycleAt(0, 16));
	source->tap().play(bank.view(), cycleAt(16, 16)); // dry, not idle
	CHECK(source->shortfallFrames() == 16);
	const auto began = Clock::now();
	writer.idle(100ms);
	CHECK(Clock::now() - began >= 100ms);
	source->tap().play(bank.view(), cycleAt(32, 16)); // dry, idle
	CHECK(source->shortfallFrames() == 16);
}

TEST_CASE("A close wakes a waiting write at once, and a write or idle after it sleeps its whole patience without "
          "spinning",
          "[source]") {
	EventSources sources;
	auto source = sources.make("rx", "from_writer", 1, 64, kRate);
	SourceWriter& writer = source->writer();
	REQUIRE(writer.write(ramp(0, 64), 64, 0ms) == 64);
	std::uint32_t accepted = ~0U;
	Clock::duration took{};
	std::thread writing([&] {
		const auto began = Clock::now();
		accepted = writer.write(ramp(64, 16), 16, 10s);
		took = Clock::now() - began;
	});
	std::this_thread::sleep_for(50ms);
	source->close();
	writing.join();
	CHECK(accepted == 0);
	CHECK(took < 5s);
	CHECK(writer.closed());
	source->close(); // idempotent

	// A library thread calling in a loop after the close: each call sleeps its patience, so 2 s cost almost no CPU.
	std::int64_t cpuNs = 0;
	int calls = 0;
	int tookSome = 0;
	Clock::duration idled{};
	std::thread looping([&] {
		const std::vector<float> chunk = ramp(0, 16);
		const std::int64_t cpuBefore = threadCpuNs();
		const auto until = Clock::now() + 2s;
		while (Clock::now() < until) {
			if (writer.write(chunk, 16, 20ms) != 0) ++tookSome;
			++calls;
		}
		cpuNs = threadCpuNs() - cpuBefore;
		const auto began = Clock::now();
		writer.idle(50ms);
		idled = Clock::now() - began;
	});
	looping.join();
	INFO(calls << " calls, " << cpuNs << " ns of CPU");
	CHECK(tookSome == 0);
	CHECK(calls <= 2000 / 20 + 1);
	CHECK(cpuNs < 20'000'000); // 1% of a core over 2 s
	CHECK(idled >= 50ms);
}

TEST_CASE("A source's played wake rings once until answered, and Played says what played", "[source]") {
	EventSources sources;
	auto source = sources.make("rx", "from_writer", 1, 64, kRate);
	SourceWriter& writer = source->writer();
	Wake& wake = writer.playedWake();
	CHECK_FALSE(readable(wake.descriptor()));
	REQUIRE(writer.write(ramp(0, 48), 48, 0ms) == 48);
	Bank bank(16);
	source->tap().play(bank.view(), cycleAt(0, 16, Latency{0, 100}));
	CHECK(readable(wake.descriptor()));
	source->tap().play(bank.view(), cycleAt(16, 16));
	source->tap().play(bank.view(), cycleAt(32, 16));
	// One write for three cycles: the eventfd's count is 1.
	std::uint64_t count = 0;
	REQUIRE(::read(wake.descriptor(), &count, sizeof count) == sizeof count);
	CHECK(count == 1);
	wake.answer();
	CHECK_FALSE(readable(wake.descriptor()));
	CHECK(writer.played() == Played{48, 32 * 1000 + 16 * 1'000'000'000LL / kRate});
	REQUIRE(writer.write(ramp(48, 16), 16, 0ms) == 16);
	source->tap().play(bank.view(), cycleAt(48, 16));
	CHECK(readable(wake.descriptor()));
	wake.answer();
	// A cycle that played nothing publishes nothing and rings nothing.
	source->tap().play(bank.view(), cycleAt(64, 16));
	CHECK_FALSE(readable(wake.descriptor()));
}

TEST_CASE("A source's play allocates nothing, a writer waiting and the played wake ringing included", "[source]") {
	EventSources sources;
	auto source = sources.make("rx", "from_writer", 1, 256, kRate);
	SourceWriter& writer = source->writer();
	REQUIRE(writer.write(ramp(0, 256), 256, 0ms) == 256);
	std::atomic<bool> stop{false};
	std::thread writing([&] {
		std::uint64_t k = 256;
		const std::vector<float> chunk = ramp(0, 64);
		while (!stop.load()) k += writer.write(chunk, 64, 20ms);
	});
	std::uint64_t allocations = ~0ULL;
	std::thread realtime([&] {
		Bank bank(64);
		const std::uint64_t before = ear::pipewire::testing::countedAllocations();
		ear::pipewire::testing::countAllocationsOnThisThread(true);
		for (std::uint64_t cycle = 0; cycle < 2000; ++cycle) {
			source->tap().play(bank.view(), cycleAt(cycle * 64, 64));
			if (cycle % 7 == 0) writer.clear();
		}
		ear::pipewire::testing::countAllocationsOnThisThread(false);
		allocations = ear::pipewire::testing::countedAllocations() - before;
	});
	realtime.join();
	stop.store(true);
	source->close();
	writing.join();
	CHECK(allocations == 0);
	CHECK(writer.played().totalFrames > 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Served by a real block on the in-process engine (docs/engine.md#sources).

namespace {

class PortLog final : public LivePortListener {
public:
	explicit PortLog(Waiter& waiter) : waiter_(waiter) {}
	void portAppeared(const LivePort& port) override {
		appeared.push_back(port);
		waiter_.wake();
	}
	void portGone(EnginePortId) override { waiter_.wake(); }
	std::optional<LivePort> find(const NodeName& node, const PortName& port) const {
		for (const LivePort& live : appeared)
			if (live.port.node == node && live.port.port == port) return live;
		return std::nullopt;
	}
	std::vector<LivePort> appeared;

private:
	Waiter& waiter_;
};

class LinkLog final : public LinkListener {
public:
	explicit LinkLog(Waiter& waiter) : waiter_(waiter) {}
	void linkActive(LinkId) override {
		++active;
		waiter_.wake();
	}
	void linkFailed(LinkId, std::string) override {
		++failed;
		waiter_.wake();
	}
	void linkGone(LinkId) override { waiter_.wake(); }
	int active = 0;
	int failed = 0;

private:
	Waiter& waiter_;
};

/// Live ports and links, as port_block_test.cpp's.
struct Wiring {
	Wiring(Engine& engine, Waiter& waiter) : live(engine), links(engine), ports(waiter), linkLog(waiter) {
		live.subscribe(ports);
		links.subscribe(linkLog);
	}
	bool link(Waiter& waiter, const PortRef& from, const PortRef& to) {
		if (!waiter.until([&] { return ports.find(from.node, from.port) && ports.find(to.node, to.port); }))
			return false;
		const int before = linkLog.active;
		links.createLink(nextLink++, ports.find(from.node, from.port)->id, ports.find(to.node, to.port)->id);
		return waiter.until([&] { return linkLog.active > before || linkLog.failed > 0; }) && linkLog.failed == 0;
	}
	LiveGraph live;
	Links links;
	PortLog ports;
	LinkLog linkLog;
	LinkId nextLink = 1;
};

/// The block "rx": a public input `in` (what an upstream feeds, unread), the source's inside input bank `from_writer`
/// and a public output `out`, its pass-through.
const Ports rxPorts{{{Direction::input, "in", {"1"}},
                     {Direction::input, "from_writer", {"1"}, PortBank::Reach::inside},
                     {Direction::output, "out", {"1"}}}};
constexpr std::size_t kInside = 1; // from_writer's index among process()'s inputs

/// Fixed storage a realtime side fills, one entry per cycle, read by the test once the count stops.
template <class Entry> class CycleLog {
public:
	explicit CycleLog(std::size_t capacity) : entries_(capacity) {}
	void add(const Entry& entry) noexcept {
		const std::size_t at = count_.load(std::memory_order_relaxed);
		if (at >= entries_.size()) return;
		entries_[at] = entry;
		count_.store(at + 1, std::memory_order_release);
	}
	std::size_t count() const noexcept { return count_.load(std::memory_order_acquire); }
	bool full() const noexcept { return count() == entries_.size(); }
	std::span<const Entry> entries() const { return {entries_.data(), count()}; }

private:
	std::vector<Entry> entries_;
	std::atomic<std::size_t> count_{0};
};

/// What the block's code saw of a cycle: its position and frames, its start, and the source's Played after it played.
struct CodeCycle {
	std::uint64_t position;
	std::uint32_t frames;
	std::int64_t startRawNs;
	Played played;
};

/// The block's code: out = from_writer (the source's bank, as the code reads it), each cycle logged with the source's
/// Played; the first position recorded for the listener downstream. It can turn on the allocation counter for the data
/// thread.
class SourceCode final : public AudioCode {
public:
	SourceCode(Waiter& waiter, const SourceWriter& writer, std::size_t capacity)
		: waiter_(waiter), writer_(writer), log(capacity) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle& cycle) noexcept override {
		ear::pipewire::testing::countAllocationsOnThisThread(countHere.load(std::memory_order_acquire));
		std::ranges::copy(inputs[kInside].first(frames), outputs[0].begin());
		if (!started.load(std::memory_order_relaxed)) {
			firstPosition.store(cycle.position, std::memory_order_relaxed);
			started.store(true, std::memory_order_release);
		}
		log.add(CodeCycle{cycle.position, frames, cycle.startRawNs, writer_.played()});
		calls.fetch_add(1, std::memory_order_release);
		waiter_.wake();
	}
	void release() noexcept override {}
	std::atomic<bool> countHere{false};
	std::atomic<bool> started{false};
	std::atomic<std::uint64_t> firstPosition{0};
	std::atomic<std::uint64_t> calls{0};

private:
	Waiter& waiter_;
	const SourceWriter& writer_;

public:
	CycleLog<CodeCycle> log;
};

/// What the listener heard of a cycle: whether every frame from the stream's first unfaded one on was the ramp frame of
/// its position, the source's first position taken as the stream's frame 0.
struct HeardCycle {
	std::uint64_t position;
	std::uint32_t checked; // frames compared (those past the fade-in)
	std::uint32_t wrong;   // of them, not the ramp's
	float first;           // the cycle's first frame
};

/// D: listens to rx's output.
class Listener final : public ProbeCode {
public:
	Listener(Waiter& waiter, const SourceCode& code, std::size_t capacity)
		: waiter_(waiter), code_(code), log(capacity) {}
	void cycle(const spa_io_clock& clock, std::span<const float* const> inputs, std::span<float* const>,
	           std::uint32_t frames) noexcept override {
		waiter_.wake();
		if (!code_.started.load(std::memory_order_acquire)) return;
		const std::uint64_t first = code_.firstPosition.load(std::memory_order_relaxed);
		HeardCycle heard{clock.position, 0, 0, inputs[0][0]};
		for (std::uint32_t i = 0; i < frames; ++i) {
			const std::uint64_t at = clock.position + i;
			if (at < first + kFadeFrames) continue;
			++heard.checked;
			if (inputs[0][i] != rampAt(at - first)) ++heard.wrong;
		}
		log.add(heard);
	}
	static constexpr std::uint32_t kFadeFrames = kRate * 5 / 1000;

private:
	Waiter& waiter_;
	const SourceCode& code_;

public:
	CycleLog<HeardCycle> log;
};

/// U: upstream of rx, logging each cycle's position; while armed it sleeps past the quantum every `every` cycles, so
/// the driver's next timeout comes before the rest of the graph ran the cycle (spike S2's cut-short cycles, made at
/// will instead of by load).
class SleepingUpstream final : public ProbeCode {
public:
	SleepingUpstream(std::size_t capacity, std::uint32_t every, std::chrono::microseconds sleep)
		: every_(every), sleep_(sleep), log(capacity) {}
	void cycle(const spa_io_clock& clock, std::span<const float* const>, std::span<float* const> outputs,
	           std::uint32_t frames) noexcept override {
		std::fill_n(outputs[0], frames, 0.0F);
		if (!armed.load(std::memory_order_acquire)) return;
		log.add(clock.position);
		if (++cycles_ % every_ == 0) {
			std::this_thread::sleep_for(sleep_); // the data thread is held past the quantum
			slept.fetch_add(1, std::memory_order_release);
		}
	}
	std::atomic<bool> armed{false};
	std::atomic<std::uint64_t> slept{0};

private:
	std::uint32_t every_;
	std::chrono::microseconds sleep_;
	std::uint64_t cycles_ = 0;

public:
	CycleLog<std::uint64_t> log;
};

/// The writer's thread: the ramp from frame 0, as fast as the ring takes it, until the source is closed (at the latest
/// by its destructor, so a test that fails early still ends).
class RampWriter {
public:
	RampWriter(Source& source, Waiter& waiter)
		: source_(source), thread_([this, &writer = source.writer(), &waiter] {
			  std::vector<float> chunk(256);
			  while (!writer.closed()) {
				  const std::uint64_t at = written.load(std::memory_order_relaxed);
				  for (std::uint32_t i = 0; i < chunk.size(); ++i) chunk[i] = rampAt(at + i);
				  const std::uint32_t took = writer.write(chunk, 256, 100ms);
				  written.store(at + took, std::memory_order_release);
				  if (took > 0) waiter.wake();
			  }
		  }) {}
	~RampWriter() {
		source_.close();
		thread_.join();
	}
	RampWriter(const RampWriter&) = delete;
	RampWriter& operator=(const RampWriter&) = delete;
	std::atomic<std::uint64_t> written{0};

private:
	Source& source_;
	std::thread thread_; // last: it reads `written`
};

} // namespace

TEST_CASE("A bound source plays its ramp into its bank before the code, reaching the public output in the same cycle, "
          "dated by the latency the block tells on its bank, and a silence unbinds it",
          "[source][blocks]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	NoBell bell;
	FilterPortBlocks blocks(*engine, bell);
	Wiring wiring(*engine, waiter);
	EventSources sources;
	auto source = sources.make("rx", "from_writer", 1, 8192, kRate);
	SourceCode code(waiter, source->writer(), 4000);
	Listener listenerCode(waiter, code, 4000);
	BlockEvents events(waiter);
	auto block = blocks.make("rx", rxPorts, kFormat, events);
	Probe listener(*engine, waiter, "listener", 1, 0, listenerCode);
	REQUIRE(wiring.link(waiter, {"rx", "out.1"}, {"listener", "in.1"}));
	REQUIRE(waiter.until([&] { return events.isReady && listener.streaming(); }));
	RampWriter writer(*source, waiter);
	REQUIRE(waiter.until([&] { return writer.written.load() >= 8192; }));
	// What the block tells on from_writer: what lies beyond its paired output (the listener reports nothing), plus 4800
	// frames the plan declares on the bank itself: the source's Played is dated by it.
	block->carryLatency(LatencyPlan{{{"from_writer", "out"}}, {{"from_writer", Latency{0, 4800}}}, {}});
	const std::vector<SourceBinding> bound{SourceBinding{&source->tap(), kInside, 1}};
	bool driven = false;
	block->drive(DriveRequest{&code, {}, {}, bound}, [&](std::uint64_t) {
		driven = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return driven; }));
	REQUIRE(waiter.until([&] { return code.log.count() >= 200 && listenerCode.log.count() >= 200; }, 10s));
	REQUIRE(ear::testing::driveNow(*block, nullptr));
	const Played stopped = source->writer().played();
	const std::size_t heardBefore = listenerCode.log.count();
	REQUIRE(waiter.until([&] { return listenerCode.log.count() >= heardBefore + 20; }));
	CHECK(source->writer().played() == stopped); // unbound: the block plays it no more
	const auto heard = listenerCode.log.entries();
	std::size_t silent = 0;
	for (const HeardCycle& cycle : heard)
		if (cycle.position > code.log.entries().back().position) {
			CHECK(cycle.first == 0.0F); // a cycle the code did not run
			++silent;
		}
	CHECK(silent >= 20);
	source->close();

	const auto cycles = code.log.entries();
	REQUIRE_FALSE(cycles.empty());
	const std::uint64_t first = code.firstPosition.load();
	const std::uint64_t last = cycles.back().position;
	std::size_t jumps = 0;
	for (std::size_t n = 0; n < cycles.size(); ++n) {
		INFO("cycle " << n << " at " << cycles[n].position);
		// Played keeps to the clock: every frame since the first position is counted, played or skipped.
		CHECK(cycles[n].played.totalFrames == cycles[n].position + cycles[n].frames - first);
		// Dated: the cycle's start, the frames it played, and the 4800 frames told downstream of its bank.
		CHECK(cycles[n].played.heardRawNs ==
		      cycles[n].startRawNs + static_cast<std::int64_t>(cycles[n].frames + 4800) * 1'000'000'000 / kRate);
		if (n > 0 && cycles[n].position != cycles[n - 1].position + cycles[n - 1].frames) ++jumps;
	}
	// The listener heard each position's ramp frame in the cycle the block played it: no offset in position.
	std::size_t checked = 0, wrong = 0;
	for (const HeardCycle& cycle : heard) {
		if (cycle.position > last) break; // the silence's
		checked += cycle.checked;
		wrong += cycle.wrong;
	}
	INFO(jumps << " position jumps in " << cycles.size() << " cycles");
	CHECK(checked > 100 * kBlock);
	CHECK(wrong == 0);
	CHECK(source->shortfallFrames() == 0);
}

TEST_CASE("A bound source, its played wake and its writer's room allocate nothing on the data thread, re-driven "
          "again and again",
          "[source][blocks]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	NoBell bell;
	FilterPortBlocks blocks(*engine, bell);
	EventSources sources;
	auto source = sources.make("rx", "from_writer", 1, 2048, kRate);
	SourceCode code(waiter, source->writer(), 0);
	BlockEvents events(waiter);
	auto block = blocks.make("rx", rxPorts, kFormat, events);
	REQUIRE(waiter.until([&] { return events.isReady; }));
	RampWriter writer(*source, waiter); // waits for room: each cycle's play writes its eventfd
	const std::vector<SourceBinding> bound{SourceBinding{&source->tap(), kInside, 1}};
	int completed = 0;
	block->drive(DriveRequest{&code, {}, {}, bound}, [&](std::uint64_t) {
		++completed;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return completed == 1 && code.calls.load() > 0; }));
	code.countHere.store(true);
	const std::uint64_t armedAt = code.calls.load();
	REQUIRE(waiter.until([&] { return code.calls.load() > armedAt; }));
	const std::uint64_t before = ear::pipewire::testing::countedAllocations();
	for (int round = 0; round < 50; ++round) {
		block->drive(DriveRequest{&code, {}, {}, bound}, [&](std::uint64_t) {
			++completed;
			waiter.wake();
		});
		const std::uint64_t calls = code.calls.load();
		REQUIRE(waiter.until([&] { return completed == round + 2 && code.calls.load() > calls + 1; }));
	}
	const std::uint64_t counted = ear::pipewire::testing::countedAllocations() - before;
	code.countHere.store(false);
	const std::uint64_t calls = code.calls.load();
	REQUIRE(waiter.until([&] { return code.calls.load() > calls + 1; }));
	REQUIRE(ear::testing::driveNow(*block, nullptr));
	source->close();
	CHECK(counted == 0);
	CHECK(source->writer().played().totalFrames > 0);
}

// Spike S5's part A on the real block (docs/engine.md#the-one-data-loop-and-invokes): a source served by its block keeps
// its played count on the clock across cycles the block did not run. An upstream probe sleeps past the quantum every
// 50th cycle, so the driver's next timeout comes while the cycle is still running: the graph runs that cycle as far as
// it got, or catches up by whole blocks, and either way the block skips positions, which its source must keep time
// over. The gate: Played's count equals the frames since the first position after every cycle the block ran, and the
// listener after the block hears each position's ramp frame.
TEST_CASE("A source on a real block keeps its played count to the clock across cycles cut short by a probe sleeping "
          "past the quantum",
          "[source][blocks][s5]") {
	constexpr std::size_t kCycles = 1500;
	auto engine = makeEngine();
	Waiter waiter(*engine);
	NoBell bell;
	FilterPortBlocks blocks(*engine, bell);
	Wiring wiring(*engine, waiter);
	EventSources sources;
	auto source = sources.make("rx", "from_writer", 1, 8192, kRate);
	SourceCode code(waiter, source->writer(), kCycles);
	Listener listenerCode(waiter, code, kCycles);
	SleepingUpstream upstreamCode(kCycles * 2, 50, 12ms); // a quantum is 5.3 ms
	BlockEvents events(waiter);
	Probe upstream(*engine, waiter, "upstream", 0, 1, upstreamCode);
	auto block = blocks.make("rx", rxPorts, kFormat, events);
	Probe listener(*engine, waiter, "listener", 1, 0, listenerCode);
	REQUIRE(wiring.link(waiter, {"upstream", "out.1"}, {"rx", "in.1"}));
	REQUIRE(wiring.link(waiter, {"rx", "out.1"}, {"listener", "in.1"}));
	REQUIRE(waiter.until([&] { return events.isReady && listener.streaming() && upstream.streaming(); }));
	RampWriter writer(*source, waiter);
	REQUIRE(waiter.until([&] { return writer.written.load() >= 8192; }));
	const std::vector<SourceBinding> bound{SourceBinding{&source->tap(), kInside, 1}};
	bool driven = false;
	upstreamCode.armed.store(true);
	block->drive(DriveRequest{&code, {}, {}, bound}, [&](std::uint64_t) {
		driven = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return driven; }));
	REQUIRE(waiter.until([&] { return code.log.full(); }, 60s));
	upstreamCode.armed.store(false);
	REQUIRE(ear::testing::driveNow(*block, nullptr));
	source->close();

	const auto cycles = code.log.entries();
	const std::uint64_t first = code.firstPosition.load();
	// Which positions the block ran, to find those its upstream ran and it did not.
	std::vector<std::uint64_t> ran;
	ran.reserve(cycles.size());
	std::size_t jumps = 0, drift = 0;
	std::uint64_t largestJump = 0;
	for (std::size_t n = 0; n < cycles.size(); ++n) {
		ran.push_back(cycles[n].position);
		if (cycles[n].played.totalFrames != cycles[n].position + cycles[n].frames - first) ++drift;
		if (n > 0 && cycles[n].position != cycles[n - 1].position + cycles[n - 1].frames) {
			++jumps;
			largestJump = std::max(largestJump, cycles[n].position - cycles[n - 1].position - cycles[n - 1].frames);
		}
	}
	std::size_t cutAfterUpstream = 0;
	const std::uint64_t last = cycles.back().position;
	for (const std::uint64_t at : upstreamCode.log.entries())
		if (at >= first && at <= last && !std::ranges::binary_search(ran, at)) ++cutAfterUpstream;
	std::size_t checked = 0, wrong = 0;
	for (const HeardCycle& heard : listenerCode.log.entries()) {
		if (heard.position > last) break;
		checked += heard.checked;
		wrong += heard.wrong;
	}
	std::printf("S5 part A on the real block: %zu cycles, %" PRIu64 " sleeps, %zu position jumps (largest %" PRIu64
	            " frames), %zu cut short after the upstream; %zu frames heard, %zu wrong; %" PRIu64 " short\n",
	            cycles.size(), upstreamCode.slept.load(), jumps, largestJump, cutAfterUpstream, checked, wrong,
	            source->shortfallFrames());
	INFO(jumps << " jumps (largest " << largestJump << "), " << cutAfterUpstream << " cut short after the upstream, "
	           << upstreamCode.slept.load() << " sleeps");
	REQUIRE(upstreamCode.slept.load() >= kCycles / 50 - 2);
	// The block missed cycles its upstream ran (cut short after it, 2026-11-09: one per sleep), which is what this
	// test is about.
	CHECK(cutAfterUpstream > 0);
	CHECK(jumps > 0);
	CHECK(largestJump <= 8192);
	CHECK(drift == 0);
	CHECK(checked > 1000 * kBlock);
	CHECK(wrong == 0);
	CHECK(source->shortfallFrames() == 0);
}
