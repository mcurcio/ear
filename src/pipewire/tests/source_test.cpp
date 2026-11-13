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
#include <limits>
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
#include "cycle_judge.hpp"
#include "inducers.hpp"
#include "probes.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace ear::pipewire::testing;
using namespace std::chrono_literals;

using Clock = std::chrono::steady_clock;
using ear::test::StallLength;
using ear::test::StallPlan;
using ear::test::Stalls;

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

/// Until a writer of `source` has begun its first wait on the room (EventSources::waitsBegun): past its last look at
/// the ring, so only a play that frees room, the close or its patience ends that wait. The writer's own event, where a
/// sleep would only guess it had come; false past 10 s, which only fails the test.
bool writerWaiting(const Source& source) {
	const auto until = Clock::now() + 10s;
	while (EventSources::waitsBegun(source) == 0) {
		if (Clock::now() >= until) return false;
		std::this_thread::yield();
	}
	return true;
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
	REQUIRE(writerWaiting(*source)); // the write found the ring full and waits for room
	CHECK_FALSE(returned.load());
	Bank bank(16);
	source->tap().play(bank.view(), cycleAt(0, 16));
	writing.join();
	CHECK(accepted == 16);
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
	REQUIRE(writerWaiting(*source));
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

TEST_CASE("A write or an idle waiting with a patience of milliseconds::max() is woken by a close at once, without "
          "spinning",
          "[source]") {
	EventSources sources;
	for (const bool idling : {false, true}) {
		INFO((idling ? "idle" : "write"));
		auto source = sources.make("rx", "from_writer", 1, 64, kRate);
		SourceWriter& writer = source->writer();
		REQUIRE(writer.write(ramp(0, 64), 64, 0ms) == 64);
		std::uint32_t accepted = ~0U;
		std::int64_t cpuNs = 0;
		Clock::duration took{};
		std::thread waiting([&] {
			const std::int64_t cpuBefore = threadCpuNs();
			const auto began = Clock::now();
			if (idling) writer.idle(std::chrono::milliseconds::max());
			else accepted = writer.write(ramp(64, 16), 16, std::chrono::milliseconds::max());
			took = Clock::now() - began;
			cpuNs = threadCpuNs() - cpuBefore;
		});
		REQUIRE(writerWaiting(*source)); // it waits, its deadline a year away
		source->close();
		waiting.join();
		if (!idling) CHECK(accepted == 0);
		// One wait, and the close (the descriptor), not its patience, ended it.
		CHECK(EventSources::waitsBegun(*source) == 1);
		CHECK(EventSources::waitsWoken(*source) == 1);
		CHECK(took < 5s);
		CHECK(cpuNs < 20'000'000);
	}
}

TEST_CASE("A wait's patience of milliseconds::max() ends a year away, never sooner and never by overflow", "[source]") {
	const auto now = Clock::now();
	const auto longest = EventSources::waitDeadline(std::chrono::milliseconds::max());
	CHECK(longest - now >= std::chrono::hours(24 * 364));
	CHECK(longest - now <= std::chrono::hours(24 * 366));
	const auto none = EventSources::waitDeadline(std::chrono::milliseconds(-5)); // clamped to no patience
	CHECK(none <= Clock::now());
	CHECK(EventSources::waitDeadline(20ms) - now >= 20ms);
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

/// What the block's code saw of a cycle: its position and frames, its start, and the source's Played after it played;
/// with a source and a writer to watch, the source's shortfall after it played, and the frames the writer had written
/// by the cycle's end (so by the next cycle's play, at least that many); and the facts its block stated of it.
struct CodeCycle {
	std::uint64_t position;
	std::uint32_t frames;
	std::int64_t startRawNs;
	Played played;
	std::uint64_t shortfall = 0;
	std::uint64_t written = 0;
	CycleFacts facts{};
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
		const std::atomic<std::uint64_t>* written = writtenBy.load(std::memory_order_acquire);
		log.add(CodeCycle{cycle.position, frames, cycle.startRawNs, writer_.played(),
		                  source != nullptr ? source->shortfallFrames() : 0,
		                  written != nullptr ? written->load(std::memory_order_acquire) : 0, cycle.facts});
		calls.fetch_add(1, std::memory_order_release);
		waiter_.wake();
	}
	void release() noexcept override {}
	std::atomic<bool> countHere{false};
	std::atomic<bool> started{false};
	std::atomic<std::uint64_t> firstPosition{0};
	std::atomic<std::uint64_t> calls{0};
	/// Logged each cycle when set: the source's shortfall, and the writer's count of frames written.
	const Source* source = nullptr;
	std::atomic<const std::atomic<std::uint64_t>*> writtenBy{nullptr};

private:
	Waiter& waiter_;
	const SourceWriter& writer_;

public:
	CycleLog<CodeCycle> log;
};

/// D, as a probe or a block: per cycle, its position, its first frame, and how many of its frames do not follow the one
/// before by one ramp step. The test judges it afterwards against what rx's source played in that cycle
/// (judgeHeard()).
struct StreamCycle {
	std::uint64_t position;
	float first;
	std::uint32_t breaks;
};
StreamCycle heardCycle(std::uint64_t position, const float* in, std::uint32_t frames) noexcept {
	std::uint32_t breaks = 0;
	for (std::uint32_t i = 1; i < frames; ++i)
		if (in[i] != in[i - 1] + 1.0F) ++breaks;
	return StreamCycle{position, in[0], breaks};
}
class StreamListener final : public ProbeCode {
public:
	StreamListener(Waiter& waiter, const SourceCode& code, std::size_t capacity)
		: waiter_(waiter), code_(code), log(capacity) {}
	void cycle(const spa_io_clock& clock, std::span<const float* const> inputs, std::span<float* const>,
	           std::uint32_t frames) noexcept override {
		waiter_.wake();
		if (!code_.started.load(std::memory_order_acquire)) return;
		log.add(heardCycle(clock.position, inputs[0], frames));
	}

private:
	Waiter& waiter_;
	const SourceCode& code_;

public:
	CycleLog<StreamCycle> log;
};

/// D as a block: listens to rx's output on its input bank `in`, whose playback latency the test declares, so rx hears
/// it from a peer as it would a following card's buffer.
class ListeningCode final : public AudioCode {
public:
	ListeningCode(Waiter& waiter, const SourceCode& code, std::size_t capacity)
		: waiter_(waiter), code_(code), log(capacity) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>>,
	             std::uint32_t frames, const GraphCycle& cycle) noexcept override {
		waiter_.wake();
		if (!code_.started.load(std::memory_order_acquire)) return;
		log.add(heardCycle(cycle.position, inputs[0].data(), frames));
	}
	void release() noexcept override {}

private:
	Waiter& waiter_;
	const SourceCode& code_;

public:
	CycleLog<StreamCycle> log;
};

/// U: upstream of rx, logging each cycle's position; while armed it stalls the data thread past the quantum at the
/// cycles its plan names (counted from the first armed cycle), so the driver's next timeout comes before the rest of
/// the graph ran the cycle (spike S2's cut-short cycles, made at will instead of by load).
class SleepingUpstream final : public ProbeCode {
public:
	SleepingUpstream(std::size_t capacity, Stalls& stalls) : stalls_(stalls), log(capacity) {}
	void cycle(const spa_io_clock& clock, std::span<const float* const>, std::span<float* const> outputs,
	           std::uint32_t frames) noexcept override {
		std::fill_n(outputs[0], frames, 0.0F);
		if (!armed.load(std::memory_order_acquire)) return;
		log.add(clock.position);
		stalls_(cycles_++, clock.duration, kRate);
	}
	std::atomic<bool> armed{false};
	std::uint64_t slept() const { return stalls_.made(); }

private:
	Stalls& stalls_;
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

/// The code's cycles judged by the source rule (docs/writing-a-node-type.md#sources), cycle by cycle: Played's count
/// moves on by the jump and the frames of each cycle; a jump past the ring's capacity is a new clock, nothing skipped (a
/// host's stall past it makes one: recorded, never required); a cycle short of frames is excused only when the writer
/// had not yet written them by the cycle before (its own progress, logged by the code: a host that starves the writer
/// thread). Each jump is the one its block stated (CycleFacts::jumpFrames); whether it was the driver's cut is the
/// judge's (docs/testing.md#the-cycle-judge).
struct SourceJudged {
	std::size_t cycles = 0, jumps = 0, beyond = 0, clocks = 0, clockNotBeyond = 0, drift = 0, lagged = 0;
	std::size_t positionOff = 0; // cycles on the same driver whose position is not the last end and the stated jump
	std::uint64_t largestJump = 0, excusedDry = 0;
	std::string firstDrift;
	/// Per cycle: the source played every frame of it, as its count says; and played it so with no dry cycle just before
	/// (whose decay and fade-in would shape its first frames).
	std::vector<std::uint8_t> played, whole;
	// What the listener heard (judgeHeard()).
	std::size_t heard = 0, judged = 0, wrong = 0;
	std::string firstWrong;
};

SourceJudged judgeSource(std::span<const CodeCycle> cycles, std::uint32_t capacity) {
	SourceJudged j;
	j.cycles = cycles.size();
	j.played.assign(cycles.size(), 0);
	j.whole.assign(cycles.size(), 0);
	bool dryBefore = true; // the first cycle fades in
	for (std::size_t n = 0; n < cycles.size(); ++n) {
		const CodeCycle& c = cycles[n];
		const CodeCycle* p = n > 0 ? &cycles[n - 1] : nullptr;
		std::uint64_t jump = 0;
		// The stated jump is the code's own view too: on the same driver, the position is the last cycle's end moved
		// by it (worked out modulo 2^64, as the facts are).
		const bool sameDriver = !c.facts.causes.has(CycleCause::start) && !c.facts.causes.has(CycleCause::driverChange);
		if (p != nullptr && sameDriver &&
		    c.position != p->position + p->frames + static_cast<std::uint64_t>(c.facts.jumpFrames))
			++j.positionOff;
		if (p != nullptr && c.facts.jumpFrames > 0) {
			++j.jumps;
			jump = static_cast<std::uint64_t>(c.facts.jumpFrames);
			j.largestJump = std::max(j.largestJump, jump);
		}
		const bool beyond = jump > capacity;
		if (beyond) ++j.beyond;
		// What the count moves by when nothing is short: the skipped jump and the frames; a new clock skips none.
		const std::uint64_t before = p != nullptr ? p->played.totalFrames : 0;
		const std::uint64_t expected = (beyond ? 0 : jump) + c.frames;
		const std::uint64_t moved = c.played.totalFrames - before;
		const std::uint64_t dry = c.shortfall - (p != nullptr ? p->shortfall : 0);
		// The writer's own progress: frames it had written by the end of the cycle before, so ready for this one.
		const bool written = p == nullptr || p->written >= before + expected;
		const bool good = moved == expected && dry == 0;
		if (!good && moved < expected && moved + dry == expected && !written) {
			++j.lagged; // the writer had not written them yet: its thread was starved; the count re-anchors
			j.excusedDry += dry;
		} else if (!good) {
			if (j.drift++ == 0)
				j.firstDrift = "cycle " + std::to_string(n) + " at " + std::to_string(c.position) + ": moved " +
				               std::to_string(moved) + ", expected " + std::to_string(expected) + ", dry " +
				               std::to_string(dry) + ", jump " + std::to_string(jump);
		}
		if (beyond && moved + dry == c.frames) ++j.clocks;
		// A count that skipped nothing over a jump within the capacity: a new clock the rule does not make.
		if (!beyond && jump > 0 && moved + dry == c.frames) ++j.clockNotBeyond;
		j.played[n] = good ? 1 : 0;
		j.whole[n] = good && !dryBefore ? 1 : 0;
		dryBefore = dry > 0;
	}
	return j;
}

/// The code's cycles from `fed` on, through `judge`, in order; `fed` moves past them.
void feedJudge(ear::test::CycleJudge& judge, std::span<const CodeCycle> cycles, std::size_t& fed) {
	for (; fed < cycles.size(); ++fed) judge.at(cycles[fed].facts, cycles[fed].frames);
}

/// The listener's cycles judged against `j`: in every cycle the source played whole and unfaded, it heard the ramp
/// frames the source's count says it played, the stream frame it began the cycle with first and no break after.
void judgeHeard(SourceJudged& j, std::span<const CodeCycle> cycles, std::span<const StreamCycle> heardLog) {
	std::vector<std::uint64_t> ran;
	ran.reserve(cycles.size());
	for (const CodeCycle& c : cycles) ran.push_back(c.position);
	for (const StreamCycle& heard : heardLog) {
		const auto at = std::ranges::lower_bound(ran, heard.position);
		if (at == ran.end() || *at != heard.position) continue; // past the code's log, or a cycle it did not run
		++j.heard;
		const auto n = static_cast<std::size_t>(at - ran.begin());
		if (j.whole[n] == 0) continue;
		++j.judged;
		const std::uint64_t from = cycles[n].played.totalFrames - cycles[n].frames;
		if (heard.first != rampAt(from) || heard.breaks != 0) {
			if (j.wrong++ == 0)
				j.firstWrong = "heard at " + std::to_string(heard.position) + ": first " + std::to_string(heard.first) +
				               ", wanted " + std::to_string(rampAt(from)) + ", " + std::to_string(heard.breaks) +
				               " breaks";
		}
	}
}

} // namespace

TEST_CASE("A bound source plays its ramp into its bank before the code, reaching the public output in the same cycle, "
          "dated by the latency downstream of its paired output and the code's own, and a silence unbinds it",
          "[source][blocks]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	NoBell bell;
	FilterPortBlocks blocks(*engine, bell);
	Wiring wiring(*engine, waiter);
	EventSources sources;
	auto source = sources.make("rx", "from_writer", 1, 8192, kRate);
	SourceCode code(waiter, source->writer(), 4000);
	code.source = source.get();
	ListeningCode listening(waiter, code, 4000);
	// The code's every cycle through the one judge (docs/testing.md#the-cycle-judge), its first its drive. An upstream
	// probe makes the judge's stalls once the code runs, each cutting its cycle before rx ran it, so the source keeps
	// time over cycles the driver cut, and the excuse is taken, and proven, on every run.
	ear::test::CycleJudge judge(ear::test::JudgePlan{.judged = 200, .expected = {.drives = 1}}, FrameClock(kRate),
	                            &engine->cuts());
	SleepingUpstream upstreamCode(4000, judge.stalls());
	Probe upstream(*engine, waiter, "upstream", 0, 1, upstreamCode);
	BlockEvents events(waiter), dstEvents(waiter);
	auto block = blocks.make("rx", rxPorts, kFormat, events);
	// The node after rx tells 960 frames of playback latency on its input, as a card's buffer would be told.
	auto dst = blocks.make("dst", Ports{{{Direction::input, "in", {"1"}}}}, kFormat, dstEvents);
	dst->carryLatency(LatencyPlan{.onBank = {{"in", Delay{{0, 960}}}}});
	REQUIRE(waiter.until([&] { return events.isReady && dstEvents.isReady; }));
	REQUIRE(ear::testing::driveNow(*dst, &listening));
	REQUIRE(wiring.link(waiter, {"rx", "out.1"}, {"dst", "in.1"}));
	REQUIRE(wiring.link(waiter, {"upstream", "out.1"}, {"rx", "in.1"}));
	RampWriter writer(*source, waiter);
	REQUIRE(waiter.until([&] { return writer.written.load() >= 8192; }));
	code.writtenBy.store(&writer.written, std::memory_order_release);
	// The code's own latency, declared as a type does, on the output it lands on: 4800 frames.
	block->carryLatency(
		LatencyPlan{.pairs = {{"from_writer", "out"}}, .onPair = {{{"from_writer", "out"}, Delay{{0, 4800}}}}});
	const std::vector<SourceBinding> bound{SourceBinding{&source->tap(), kInside, 1}};
	bool driven = false;
	block->drive(DriveRequest{&code, {}, {}, bound}, [&](std::uint64_t) {
		driven = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return driven; }));
	upstreamCode.armed.store(true);
	// Dated by the cycle's start, the frames it played, dst's 960 beyond `out` and the code's 4800 on it.
	const auto heardAt = [](const CodeCycle& cycle, std::uint64_t played, std::uint32_t downstream) {
		return cycle.startRawNs + static_cast<std::int64_t>(played + downstream) * 1'000'000'000 / kRate;
	};
	const auto dated = [&] {
		const auto cycles = code.log.entries();
		return !cycles.empty() &&
		       cycles.back().played.heardRawNs == heardAt(cycles.back(), cycles.back().frames, 960 + 4800);
	};
	REQUIRE(waiter.until(dated)); // once dst's latency reached rx
	const std::size_t datedFrom = code.log.count() - 1;
	// The source keeps to the clock by its rule, judged cycle by cycle as part A's is (judgeSource): a host that starves
	// the writer thread or stalls the data thread past the ring only makes a cycle that is not judged against dst, never
	// a pass or a failure. dst is judged on the cycles the source played whole, and the date with both latencies on
	// those it played every frame of once dst's latency arrived, each to a planned count; and the judge to its own.
	std::size_t fed = 0;
	constexpr std::size_t kJudged = 200;
	const auto judgeAll = [&] {
		const auto cycles = code.log.entries();
		SourceJudged j = judgeSource(cycles, source->writer().capacityFrames());
		judgeHeard(j, cycles, listening.log.entries());
		return j;
	};
	const auto playedSinceDated = [&](const SourceJudged& j) {
		return static_cast<std::size_t>(
			std::count(j.played.begin() + static_cast<std::ptrdiff_t>(datedFrom), j.played.end(), std::uint8_t{1}));
	};
	REQUIRE(waiter.until(
		[&] {
			if (code.log.full()) return true;
			if (code.log.count() <= datedFrom) return false;
			feedJudge(judge, code.log.entries(), fed);
			if (judge.report().failed > 0) return true;
			const SourceJudged j = judgeAll();
			return j.judged >= kJudged && playedSinceDated(j) >= kJudged && judge.done();
		},
		30s));
	{
		INFO("the code's log filled its " << code.log.entries().size() << " cycles before " << kJudged
		                                  << " cycles were judged");
		REQUIRE_FALSE(code.log.full());
	}
	upstreamCode.armed.store(false);
	REQUIRE(ear::testing::driveNow(*block, nullptr));
	const Played stopped = source->writer().played();
	const std::size_t heardBefore = listening.log.count();
	REQUIRE(waiter.until([&] { return listening.log.count() >= heardBefore + 20; }));
	CHECK(source->writer().played() == stopped); // unbound: the block plays it no more
	REQUIRE(ear::testing::driveNow(*dst, nullptr));
	source->close();

	const auto cycles = code.log.entries();
	REQUIRE_FALSE(cycles.empty());
	const std::uint64_t last = cycles.back().position;
	const SourceJudged j = judgeSource(cycles, source->writer().capacityFrames());
	// Played is dated by the last frame the cycle played, so a cycle the writer left short (a starved writer thread) is
	// dated earlier by the frames it lacked, its shortfall; one that moved the count not at all keeps the last date.
	for (std::size_t n = 0; n < cycles.size(); ++n) {
		const CodeCycle& c = cycles[n];
		INFO("cycle " << n << " at " << c.position << ", played every frame: " << int{j.played[n]});
		const CodeCycle* p = n > 0 ? &cycles[n - 1] : nullptr;
		if (p != nullptr && c.played.totalFrames == p->played.totalFrames) {
			CHECK(c.played.heardRawNs == p->played.heardRawNs);
			continue;
		}
		const std::uint64_t dry = c.shortfall - (p != nullptr ? p->shortfall : 0);
		const std::uint64_t played = c.frames - std::min<std::uint64_t>(dry, c.frames);
		// Before dst's latency arrived, the code's own alone; from then on, both.
		if (n >= datedFrom) CHECK(c.played.heardRawNs == heardAt(c, played, 960 + 4800));
		else
			CHECK((c.played.heardRawNs == heardAt(c, played, 4800) ||
			       c.played.heardRawNs == heardAt(c, played, 960 + 4800)));
	}
	CHECK(playedSinceDated(j) >= kJudged);
	// Played keeps to the clock by the source rule, and dst heard, in each cycle the source played whole, the ramp
	// frames its count says it played: no offset in position. Then silence.
	SourceJudged heard = j;
	judgeHeard(heard, cycles, listening.log.entries());
	std::size_t silent = 0;
	for (const StreamCycle& cycle : listening.log.entries()) {
		if (cycle.position <= last) continue;
		CHECK(cycle.first == 0.0F); // a cycle the code did not run
		++silent;
	}
	INFO(j.jumps << " position jumps in " << cycles.size() << " cycles (largest " << j.largestJump << ", " << j.beyond
	             << " beyond the ring: " << j.clocks << " new clocks); " << j.lagged
	             << " cycles the writer had not filled (" << j.excusedDry << " frames short)");
	INFO(j.firstDrift);
	INFO(heard.firstWrong);
	feedJudge(judge, cycles, fed);
	INFO(judge.report());
	CHECK(judge.report().ok());
	CHECK(j.positionOff == 0);
	CHECK(j.clocks == j.beyond);
	CHECK(j.clockNotBeyond == 0);
	CHECK(j.drift == 0);
	// Every frame short was one the writer had not written, and the source counted no other.
	CHECK(source->shortfallFrames() == cycles.back().shortfall);
	CHECK(cycles.back().shortfall == j.excusedDry);
	CHECK(silent >= 20);
	CHECK(heard.judged >= kJudged);
	CHECK(heard.wrong == 0);
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
// over. The gate, cycle by cycle, by the source rule (docs/writing-a-node-type.md#sources): Played's count moves on by
// the jump and the frames of each cycle; a jump past the ring's capacity is a new clock, nothing skipped (a host's
// stall past 170 ms makes one; recorded, never required); and a cycle short of frames is excused only when the writer
// had not yet written them by the cycle before (its own progress, logged by the code: a host that starves the writer
// thread), never otherwise. The listener after the block hears, in every cycle the source played whole and unfaded,
// the ramp frames the source's count says it played.
TEST_CASE("A source on a real block keeps its played count to the clock across cycles cut short by a probe sleeping "
          "past the quantum",
          "[source][blocks][s5]") {
	constexpr std::size_t kCycles = 1500;   // the cycles run before judging
	constexpr std::size_t kJudged = 1000;   // listener cycles judged against the ramp, planned
	constexpr std::size_t kCapacity = 6000; // only fails the test, if the judged ones never come
	constexpr std::uint32_t kRing = 8192;
	auto engine = makeEngine();
	Waiter waiter(*engine);
	NoBell bell;
	FilterPortBlocks blocks(*engine, bell);
	Wiring wiring(*engine, waiter);
	EventSources sources;
	auto source = sources.make("rx", "from_writer", 1, kRing, kRate);
	const std::uint32_t capacity = source->writer().capacityFrames();
	SourceCode code(waiter, source->writer(), kCapacity);
	code.source = source.get();
	StreamListener listenerCode(waiter, code, kCapacity);
	// The code's every cycle through the one judge (docs/testing.md#the-cycle-judge), its first its drive; its stalls
	// are the upstream's, every 50th armed cycle, the 50th first, held 12 ms (a quantum is 5.3 ms), 30 of them.
	ear::test::CycleJudge judge(ear::test::JudgePlan{.judged = kJudged,
	                                                 .induce = StallPlan::every(50, 49, 30, StallLength::of(12ms)),
	                                                 .expected = {.drives = 1}},
	                            FrameClock(kRate), &engine->cuts());
	SleepingUpstream upstreamCode(kCapacity * 2, judge.stalls());
	BlockEvents events(waiter);
	Probe upstream(*engine, waiter, "upstream", 0, 1, upstreamCode);
	auto block = blocks.make("rx", rxPorts, kFormat, events);
	Probe listener(*engine, waiter, "listener", 1, 0, listenerCode);
	REQUIRE(wiring.link(waiter, {"upstream", "out.1"}, {"rx", "in.1"}));
	REQUIRE(wiring.link(waiter, {"rx", "out.1"}, {"listener", "in.1"}));
	REQUIRE(waiter.until([&] { return events.isReady && listener.streaming() && upstream.streaming(); }));
	RampWriter writer(*source, waiter);
	REQUIRE(waiter.until([&] { return writer.written.load() >= kRing; }));
	code.writtenBy.store(&writer.written, std::memory_order_release);
	const std::vector<SourceBinding> bound{SourceBinding{&source->tap(), kInside, 1}};
	bool driven = false;
	block->drive(DriveRequest{&code, {}, {}, bound}, [&](std::uint64_t) {
		driven = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return driven; }));
	upstreamCode.armed.store(true);

	// Judges every cycle logged so far (both logs only grow; a snapshot is whole up to its count).
	const auto judgeAll = [&] {
		const auto cycles = code.log.entries();
		SourceJudged j = judgeSource(cycles, capacity);
		judgeHeard(j, cycles, listenerCode.log.entries());
		return j;
	};
	std::size_t fed = 0;
	REQUIRE(waiter.until([&] { return code.log.count() >= kCycles; }, 60s));
	SourceJudged j = judgeAll();
	feedJudge(judge, code.log.entries(), fed);
	while ((j.judged < kJudged || !judge.done()) && !code.log.full() && judge.report().failed == 0) {
		const std::size_t more = code.log.count() + 500;
		REQUIRE(waiter.until([&] { return code.log.count() >= std::min(more, kCapacity); }, 60s));
		j = judgeAll();
		feedJudge(judge, code.log.entries(), fed);
	}
	upstreamCode.armed.store(false);
	REQUIRE(ear::testing::driveNow(*block, nullptr));
	source->close();
	j = judgeAll();
	feedJudge(judge, code.log.entries(), fed);

	const auto cycles = code.log.entries();
	const std::uint64_t first = code.firstPosition.load();
	std::size_t cutAfterUpstream = 0;
	const std::uint64_t last = cycles.back().position;
	std::vector<std::uint64_t> ran;
	for (const CodeCycle& c : cycles) ran.push_back(c.position);
	for (const std::uint64_t at : upstreamCode.log.entries())
		if (at >= first && at <= last && !std::ranges::binary_search(ran, at)) ++cutAfterUpstream;
	std::printf("S5 part A on the real block: %zu cycles, %" PRIu64 " sleeps, %zu position jumps (largest %" PRIu64
	            " frames, %zu beyond the ring's %u: %zu new clocks), %zu cut short after the upstream; %zu cycles the "
	            "writer had not filled (%" PRIu64 " frames short); %zu heard, %zu judged, %zu wrong\n",
	            j.cycles, upstreamCode.slept(), j.jumps, j.largestJump, j.beyond, capacity, j.clocks, cutAfterUpstream,
	            j.lagged, j.excusedDry, j.heard, j.judged, j.wrong);
	INFO(j.jumps << " jumps (largest " << j.largestJump << "), " << cutAfterUpstream
	             << " cut short after the upstream, " << upstreamCode.slept() << " sleeps");
	INFO(j.firstDrift);
	INFO(j.firstWrong);
	INFO(judge.report());
	REQUIRE(upstreamCode.slept() == 30);
	// The block missed cycles its upstream ran (cut short after it, 2026-11-09: one per sleep), which is what this
	// test is about.
	CHECK(cutAfterUpstream > 0);
	CHECK(j.jumps > 0);
	// Every jump was cycles the driver cut, each within the time the raw clock lost, by the one judge; and every jump
	// past the capacity, and nothing else, was a new clock to the source.
	CHECK(judge.report().ok());
	CHECK(j.positionOff == 0);
	CHECK(j.clocks == j.beyond);
	CHECK(j.clockNotBeyond == 0);
	CHECK(j.drift == 0);
	// Every frame short was one the writer had not written; the source's own count of them agrees.
	CHECK(source->shortfallFrames() >= j.excusedDry);
	CHECK(j.judged >= kJudged);
	CHECK(j.wrong == 0);
}
