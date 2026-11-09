// PipeWire spikes, kept as regressions (docs/engine.md#the-one-data-loop-and-invokes, what they settled), on the real
// in-process engine:
//  - S1: pw_loop_invoke from the main loop without blocking: where, when and in what order the functions run,
//    their completions back through a main-loop event, nothing allocated on the data thread, what a burst costs, and a
//    stalled data loop (one process() asleep 2 s). PipeWire 1.4.2 (spa/plugins/support/loop.c) never refuses or waits
//    on a full invoke queue: the posting thread allocates an overflow queue and carries on. A loop with no thread
//    (not yet entered, or stopped) runs the function inline on the caller. A function may run before its post returns.
//  - S2: take-up by position (docs/engine.md#drive-and-fade) under an invoke storm: a block and its peers see one
//    clock.position per cycle, in link order, and a ramp switched on and off by position arrives sample-exact. An xrun
//    cuts a cycle short: only a prefix of the graph runs it. Positions no longer step back (engine.conf's resync.ms),
//    but cycles are still told apart by the order they ran in, never by position.
//  - S3: one data loop: context.num-data-loops = 1 in engine.conf, and every engine node on that loop. A
//    pw_filter's node info names no loop (node.loop.name is only what its maker set), so its loop is read in-process.
//  - S5: a source served by its block, as slice 3b designs it (docs/design/companions.md), on S2's harness beside a
//    busy thread per core, its upstream and itself overrunning chosen cycles so that both shapes of a cut-short cycle
//    occur on any host: its played count keeps to the clock across cycles an xrun cuts short, and the node after it
//    hears its frame of each position. And what a node reads after its upstream ran a cycle it did not: the upstream's
//    frames of the cycle it runs, never the cut one's (docs/engine.md#the-one-data-loop-and-invokes).
// Everything here is test-only: the probes are bare pw_filters (probes.hpp, shared with port_block_test.cpp), S2's
// take-up block implements a deferred take-up itself (FilterPortBlock applies its drive in its invoke instead,
// docs/engine.md#drive-and-fade), and S5's source keeps its ring itself. Every wait is event-gated: the main loop runs
// until the events a test waits for have arrived; a deadline only fails the test. Measurements are printed on standard
// output (run the binary, or ctest -V, to see them).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include "allocation_count.hpp"
#include "catch2/catch.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/links.hpp"
#include "ear/pipewire/live_graph.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/rtp_receivers.hpp"
#include "ear/pipewire/sound_cards.hpp"
#include "ear/testing/drive_now.hpp"
#include "engine_harness.hpp"
#include "inducers.hpp"
#include "probes.hpp"
#include "rtp_tone.hpp"
#include "temp_folder.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace ear::pipewire::testing;
using namespace std::chrono_literals;
using ear::test::StallLength;
using ear::test::StallPlan;
using ear::test::Stalls;

class PortLog final : public LivePortListener {
public:
	explicit PortLog(Waiter& waiter) : waiter_(waiter) {}
	void portAppeared(const LivePort& port) override {
		appeared.push_back(port);
		waiter_.wake();
	}
	void portGone(EnginePortId) override { waiter_.wake(); }
	std::optional<LivePort> find(const NodeName& node, const PortName& port) const {
		for (const LivePort& live : appeared) {
			if (live.port.node == node && live.port.port == port) return live;
		}
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
		if (!waiter.until([&] { return ports.find(from.node, from.port) && ports.find(to.node, to.port); })) {
			return false;
		}
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

/// The context's data loop: with context.num-data-loops = 1 (S3) the loop of every block.
pw_loop* contextDataLoop(Engine& engine) { return pw_data_loop_get_loop(pw_context_get_data_loop(engine.context())); }

double millisecondsSince(std::chrono::steady_clock::time_point start) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------------------------------------------------------------------
// S1

/// What S1 shares between the main loop, the blocks' code and the invoked functions. The data thread writes only into
/// storage made beforehand.
struct InvokeRecord {
	static constexpr std::uint64_t kNormal = 10'000; // in bursts of 1,000 and singly, the data loop running
	static constexpr std::uint64_t kStalled = 3'000; // while one block's process sleeps 2 s
	static constexpr std::uint64_t kCount = kNormal + kStalled;
	static constexpr int kFunctionResult = 7; // what each function returns: a blocking invoke would return it

	InvokeRecord(Waiter& waiter, pw_loop* mainLoop)
		: waiter(waiter), mainLoop(mainLoop), order(kCount, 0), onDataThread(kCount, 0), duringProcess(kCount, 0),
		  async(kCount, 0) {
		completion = pw_loop_add_event(mainLoop, &InvokeRecord::onCompletion, this);
	}
	~InvokeRecord() { pw_loop_destroy_source(mainLoop, completion); }
	InvokeRecord(const InvokeRecord&) = delete;
	InvokeRecord& operator=(const InvokeRecord&) = delete;

	/// Main loop: a completion signalled from the data thread. Signals coalesce; the number does not.
	static void onCompletion(void* data, std::uint64_t /*count*/) {
		auto* self = static_cast<InvokeRecord*>(data);
		++self->completionEvents;
		self->seen = self->completed.load(std::memory_order_acquire);
		self->waiter.wake();
	}

	/// Data thread: one invoked function.
	static int onInvoked(spa_loop* /*loop*/, bool isAsync, std::uint32_t /*seq*/, const void* data, std::size_t size,
	                     void* userData) {
		auto* self = static_cast<InvokeRecord*>(userData);
		std::uint64_t number = 0;
		if (size == sizeof(number)) std::memcpy(&number, data, sizeof(number));
		if (number == 1) countAllocationsOnThisThread(true);
		const std::size_t at = self->ran.load(std::memory_order_relaxed);
		if (at < kCount) {
			self->order[at] = number;
			self->onDataThread[at] = self->dataThreadKnown.load(std::memory_order_acquire) &&
			                         pthread_equal(pthread_self(), self->dataThread) != 0;
			self->duringProcess[at] = self->insideProcess.load(std::memory_order_acquire) != 0 ? 1 : 0;
			self->async[at] = isAsync ? 1 : 0;
		}
		self->ran.store(at + 1, std::memory_order_release);
		self->completed.store(number, std::memory_order_release);
		pw_loop_signal_event(self->mainLoop, self->completion);
		if (number == kCount) countAllocationsOnThisThread(false);
		return kFunctionResult;
	}

	/// Data thread: every block's code, on entry and on exit.
	void enter() noexcept {
		insideProcess.fetch_add(1, std::memory_order_acq_rel);
		if (!dataThreadKnown.load(std::memory_order_relaxed)) {
			dataThread = pthread_self();
			dataThreadKnown.store(true, std::memory_order_release);
		}
	}
	void leave() noexcept {
		cycles.fetch_add(1, std::memory_order_relaxed);
		insideProcess.fetch_sub(1, std::memory_order_acq_rel);
		waiter.wake();
	}

	Waiter& waiter;
	pw_loop* mainLoop;
	spa_source* completion = nullptr;
	// Data thread.
	std::vector<std::uint64_t> order;        // the numbers in the order the functions ran
	std::vector<std::uint8_t> onDataThread;  // per run: the function ran on the thread that runs process()
	std::vector<std::uint8_t> duringProcess; // per run: some block's process() was running at that moment
	std::vector<std::uint8_t> async;         // per run: the loop told the function it ran asynchronously
	std::atomic<std::size_t> ran{0};
	std::atomic<std::uint64_t> completed{0};
	std::atomic<int> insideProcess{0};
	std::atomic<std::uint64_t> cycles{0};
	pthread_t dataThread{};
	std::atomic<bool> dataThreadKnown{false};
	// Main loop.
	std::uint64_t seen = 0; // the newest completion the main loop has heard through the event
	std::uint64_t completionEvents = 0;
};

/// A block's code (FilterPortBlock's AudioCode) that flags "inside process" and counts cycles.
class FlaggedCode final : public AudioCode {
public:
	explicit FlaggedCode(InvokeRecord& record) : record_(record) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {
		record_.enter();
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
		record_.leave();
	}
	void release() noexcept override {}

private:
	InvokeRecord& record_;
};

/// A block's code that, once armed, sleeps 2 s inside one process(): the data loop stalled, as a wedged card or a
/// page fault on a loaded host would stall it.
class StallingCode final : public AudioCode {
public:
	explicit StallingCode(InvokeRecord& record) : record_(record) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {
		record_.enter();
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
		if (armed.exchange(false, std::memory_order_acq_rel)) {
			stalling.store(true, std::memory_order_release);
			record_.waiter.wake();
			const timespec twoSeconds{2, 0};
			nanosleep(&twoSeconds, nullptr);
			stalling.store(false, std::memory_order_release);
			stalled.store(true, std::memory_order_release);
		}
		record_.leave();
	}
	void release() noexcept override {}
	std::atomic<bool> armed{false};
	std::atomic<bool> stalling{false};
	std::atomic<bool> stalled{false};

private:
	InvokeRecord& record_;
};

/// The same for a probe.
class FlaggedProbe final : public ProbeCode {
public:
	explicit FlaggedProbe(InvokeRecord& record) : record_(record) {}
	void cycle(const spa_io_clock&, std::span<const float* const>, std::span<float* const> outputs,
	           std::uint32_t frames) noexcept override {
		record_.enter();
		for (float* out : outputs) std::fill_n(out, frames, 0.0F);
		record_.leave();
	}

private:
	InvokeRecord& record_;
};

/// Posts S1's invokes: each from the main thread, without blocking, its number as seq and as data.
class Poster {
public:
	Poster(InvokeRecord& record, pw_loop* dataLoop, pw_loop* mainLoop)
		: record_(record), dataLoop_(dataLoop), mainLoop_(mainLoop), results_(InvokeRecord::kCount, 0) {
		single_ = pw_loop_add_event(mainLoop, &Poster::onSingle, this);
	}
	~Poster() { pw_loop_destroy_source(mainLoop_, single_); }
	Poster(const Poster&) = delete;
	Poster& operator=(const Poster&) = delete;

	/// Numbers [first, last] in one go; returns what the whole burst cost the main loop, in ms.
	double burst(std::uint64_t first, std::uint64_t last) {
		const auto start = std::chrono::steady_clock::now();
		for (std::uint64_t number = first; number <= last; ++number) post(number);
		return millisecondsSince(start);
	}
	/// Numbers [first, last], one per main-loop turn (each turn posts one and signals the next turn).
	void singly(std::uint64_t first, std::uint64_t last) {
		next_ = first;
		last_ = last;
		pw_loop_signal_event(mainLoop_, single_);
	}

	/// One number now; returns what the post cost, in ms.
	double one(std::uint64_t number) {
		const auto start = std::chrono::steady_clock::now();
		post(number);
		return millisecondsSince(start);
	}

	std::span<const int> results() const { return results_; }
	std::uint64_t ranBeforeReturn() const { return ranBeforeReturn_; }
	double slowestSingleMs() const { return slowestSingleMs_; }

private:
	void post(std::uint64_t number) {
		results_[number - 1] = pw_loop_invoke(dataLoop_, &InvokeRecord::onInvoked, static_cast<std::uint32_t>(number),
		                                      &number, sizeof(number), false, &record_);
		// The functions run in order (checked below), so function `number` ran iff `ran` reached it.
		if (record_.ran.load(std::memory_order_acquire) >= number) ++ranBeforeReturn_;
	}
	static void onSingle(void* data, std::uint64_t /*count*/) {
		auto* self = static_cast<Poster*>(data);
		if (self->next_ > self->last_) return;
		const auto start = std::chrono::steady_clock::now();
		self->post(self->next_++);
		self->slowestSingleMs_ = std::max(self->slowestSingleMs_, millisecondsSince(start));
		if (self->next_ <= self->last_) pw_loop_signal_event(self->mainLoop_, self->single_);
	}

	InvokeRecord& record_;
	pw_loop* dataLoop_;
	pw_loop* mainLoop_;
	spa_source* single_ = nullptr;
	std::vector<int> results_;
	std::uint64_t next_ = 1;
	std::uint64_t last_ = 0;
	std::uint64_t ranBeforeReturn_ = 0;
	double slowestSingleMs_ = 0.0;
};

// ---------------------------------------------------------------------------------------------------------------------
// S2

/// One value per frame of the graph clock: nonzero, exact as a float, and different for every frame within 2^24.
float rampAt(std::uint64_t frame) noexcept { return static_cast<float>((frame & 0xFFFFFFU) + 1U); }

/// One entry per recorded cycle: its clock.position and the shared entry sequence number it took on entry.
struct CycleLog {
	explicit CycleLog(std::size_t capacity) : position(capacity), entry(capacity) {}
	/// Data thread; returns the cycle's index, or none when full.
	std::optional<std::size_t> add(std::uint64_t at, std::uint64_t sequence) noexcept {
		const std::size_t n = count.load(std::memory_order_relaxed);
		if (n >= position.size()) {
			full.store(true, std::memory_order_relaxed);
			return std::nullopt;
		}
		position[n] = at;
		entry[n] = sequence;
		return n;
	}
	void commit(std::size_t n) noexcept { count.store(n + 1, std::memory_order_release); }
	std::vector<std::uint64_t> position;
	std::vector<std::uint64_t> entry;
	std::atomic<std::size_t> count{0};
	std::atomic<bool> full{false}; // a cycle was not recorded: the log is incomplete and S2's analysis void
};

/// What S2's three filters share: the entry sequence and whether cycles are recorded.
struct Shared {
	std::atomic<std::uint64_t> sequence{0};
	std::atomic<bool> recording{false};
};

constexpr std::size_t kS2Requests = 1000;
/// S2 runs at a 64-frame block (1.3 ms a cycle at 48 kHz): its 1,000 requests 1 to 30 cycles apart take about 18,000
/// cycles whatever the block, so this keeps it near 25 s where 256 frames take 95 s; nothing it checks depends on the
/// block size.
constexpr std::uint32_t kS2Block = 64;

/// The in-process engine at the graph frequency and `block`, as engine_harness.hpp's makeEngine.
std::unique_ptr<Engine> engineAt(std::uint32_t block) {
	for (const char* name : {"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE", "DBUS_SESSION_BUS_ADDRESS",
	                         "DBUS_SYSTEM_BUS_ADDRESS"}) {
		unsetenv(name);
	}
	auto engine = Engine::create(EngineSettings{EAR_ENGINE_CONF, kRate, block});
	if (!engine) FAIL("Engine::create: " << engine.error().detail);
	return std::move(*engine);
}
/// Cycles a log holds: at most 30 between requests, plus take-ups, plus the cycles a starved data loop runs only U for
/// (under load the driver starts cycles faster than the graph finishes them).
constexpr std::size_t kS2Capacity = kS2Requests * 80 + 8000;

/// 10 ms in frames: node-driver.c's default resync.ms. engine.conf sets resync.ms beyond any stall, so the driver never
/// resets, and its loop keeps the position within its 1 ms error clamp of the raw clock: never this far ahead of it.
constexpr std::uint64_t kResyncFrames = kRate / 100;

/// CLOCK_MONOTONIC_RAW now, in frames of the graph frequency: the clock the dummy driver tracks (engine.conf's
/// clock.id), on the scale of its clock.position (node-driver.c's current_position).
std::uint64_t rawFramesNow() noexcept {
	timespec now{};
	clock_gettime(CLOCK_MONOTONIC_RAW, &now);
	return static_cast<std::uint64_t>(now.tv_sec) * kRate +
	       static_cast<std::uint64_t>(now.tv_nsec) * kRate / 1'000'000'000U;
}

/// Which recorded cycles a probe overruns (S5): those whose index in its log is `phase` modulo `every`, the first
/// `limit` of them, each by one and a half of the clock's cycles slept inside the probe's process, so the driver's next
/// timeout expires before the probe returns. Every node on the one data loop is woken by an eventfd its upstream writes
/// as it finishes, and the loop dispatches ready sources in the order they became ready (spa's loop_iterate over
/// epoll), so the timer's is dispatched first: node_ready (impl-node.c) starts the next cycle and resets the node this
/// probe just triggered, whose wake then finds it no longer triggered. That cycle is cut short straight after this
/// probe, whatever the host's load. The half cycle beyond the timeout is a margin; the driver may cut a cycle or two
/// more as it catches up, which S5 counts with the host's own.
StallPlan overruns(std::size_t every, std::size_t phase, std::size_t limit) {
	return StallPlan::every(every, phase, limit, StallLength::cycles(1.5));
}

/// U: records its cycles, with the raw clock on entry, and stamps every frame it writes with its cycle's position
/// (rampAt(position)), so a node downstream can tell which of U's cycles it reads (S5's part B; S2's B ignores it).
/// With `overruns` (S5), it overruns those of its recorded cycles once it has written its output.
class Upstream final : public ProbeCode {
public:
	explicit Upstream(Shared& shared, std::size_t capacity = kS2Capacity, StallPlan overruns = {})
		: shared_(shared), overruns_(std::move(overruns)), log(capacity), rawAtEntry(capacity), overran(capacity) {}
	void cycle(const spa_io_clock& clock, std::span<const float* const>, std::span<float* const> outputs,
	           std::uint32_t frames) noexcept override {
		const std::uint64_t sequence = shared_.sequence.fetch_add(1, std::memory_order_acq_rel);
		const std::uint64_t raw = rawFramesNow();
		std::fill_n(outputs[0], frames, rampAt(clock.position));
		if (!shared_.recording.load(std::memory_order_acquire)) return;
		if (const auto n = log.add(clock.position, sequence)) {
			rawAtEntry[*n] = raw;
			overran[*n] = overruns_.plan().planned(*n) ? 1 : 0;
			log.commit(*n);
			overruns_(*n, clock.duration, kRate);
		}
	}
	std::size_t overrunCount() const { return overruns_.made(); }

private:
	Shared& shared_;
	Stalls overruns_;

public:
	CycleLog log;
	std::vector<std::uint64_t> rawAtEntry;
	std::vector<std::uint8_t> overran; // per cycle: U overran it
};

/// B: docs/engine.md#drive-and-fade take-up by position, test-only. A request (drive: the ramp; silence: zeros) arrives
/// by a non-blocking invoke into `pending`; the next cycle that sees it publishes takenAt = position + duration and
/// runs the old code once more; the cycle whose position reaches takenAt swaps the code (a cycle that jumped past it,
/// at once; after a step back, once the position reaches it again) and completes the request: its number into
/// `applied`, then a main-loop signal.
class TakeUp final : public ProbeCode {
public:
	struct Request {
		std::uint64_t number;
		bool drive;
	};
	struct Taken {
		std::uint64_t number;
		bool drive;
		std::uint64_t publishedAt; // the position of the cycle that published takenAt
		std::uint64_t takenAt;
		std::uint64_t swappedAt;    // the position of the first cycle that ran the new code
		std::uint64_t publishedSeq; // B's entry sequence number in the cycle that published takenAt
		std::uint64_t swappedSeq;   // and in the first cycle that ran the new code
	};

	TakeUp(Shared& shared, Waiter& waiter) : shared_(shared), waiter_(waiter), log(kS2Capacity), taken_(kS2Requests) {}

	/// Main thread: returns pw_loop_invoke's result.
	int post(pw_loop* dataLoop, Request request) {
		return pw_loop_invoke(dataLoop, &TakeUp::onRequest, SPA_ID_INVALID, &request, sizeof(request), false, this);
	}
	std::uint64_t applied() const { return applied_.load(std::memory_order_acquire); }
	std::uint64_t cycles() const { return cycles_.load(std::memory_order_acquire); }
	std::uint64_t takenAt() const { return takenAt_.load(std::memory_order_acquire); }
	std::span<const Taken> taken() const { return {taken_.data(), takenCount_.load(std::memory_order_acquire)}; }
	std::uint64_t overlaps() const { return overlaps_.load(std::memory_order_acquire); }
	std::uint64_t jumps() const { return jumps_.load(std::memory_order_acquire); }
	std::uint64_t backwardJumps() const { return backwardJumps_.load(std::memory_order_acquire); }

	void cycle(const spa_io_clock& clock, std::span<const float* const>, std::span<float* const> outputs,
	           std::uint32_t frames) noexcept override {
		const std::uint64_t sequence = shared_.sequence.fetch_add(1, std::memory_order_acq_rel);
		const std::uint64_t at = clock.position;
		if (cycles_.load(std::memory_order_relaxed) > 0 && at != next_) {
			jumps_.fetch_add(1, std::memory_order_release);
			if (at < next_) backwardJumps_.fetch_add(1, std::memory_order_relaxed);
		}
		next_ = at + clock.duration;
		if (hasPending_) {
			if (!published_) {
				published_ = true;
				publishedAt_ = at;
				publishedSeq_ = sequence;
				takenAtLocal_ = at + clock.duration;
				takenAt_.store(takenAtLocal_, std::memory_order_release);
			} else if (at >= takenAtLocal_) {
				driving_ = pending_.drive;
				const std::size_t n = takenCount_.load(std::memory_order_relaxed);
				if (n < taken_.size()) {
					taken_[n] = Taken{pending_.number, pending_.drive, publishedAt_, takenAtLocal_, at,
					                  publishedSeq_,   sequence};
					takenCount_.store(n + 1, std::memory_order_release);
				}
				hasPending_ = false;
				published_ = false;
				applied_.store(pending_.number, std::memory_order_release);
			}
		}
		for (std::uint32_t i = 0; i < frames; ++i) outputs[0][i] = driving_ ? rampAt(at + i) : 0.0F;
		if (shared_.recording.load(std::memory_order_acquire)) {
			if (const auto n = log.add(at, sequence)) log.commit(*n);
		}
		cycles_.fetch_add(1, std::memory_order_release);
		waiter_.wake(); // every cycle, and so every completion, reaches the main loop through one event
	}

private:
	/// Data loop, between cycles.
	static int onRequest(spa_loop*, bool, std::uint32_t, const void* data, std::size_t size, void* userData) {
		auto* self = static_cast<TakeUp*>(userData);
		if (size != sizeof(Request)) return 0;
		if (self->hasPending_) self->overlaps_.fetch_add(1, std::memory_order_relaxed);
		std::memcpy(&self->pending_, data, sizeof(Request));
		self->hasPending_ = true;
		return 0;
	}

	Shared& shared_;
	Waiter& waiter_;

public:
	CycleLog log;

private:
	// Data thread only.
	Request pending_{};
	bool hasPending_ = false;
	bool published_ = false;
	bool driving_ = false;
	std::uint64_t publishedAt_ = 0;
	std::uint64_t publishedSeq_ = 0;
	std::uint64_t takenAtLocal_ = 0;
	std::uint64_t next_ = 0;
	// Read elsewhere.
	std::atomic<std::uint64_t> takenAt_{0};
	std::atomic<std::uint64_t> applied_{0};
	std::atomic<std::uint64_t> cycles_{0};
	std::atomic<std::uint64_t> overlaps_{0};
	std::atomic<std::uint64_t> jumps_{0};
	std::atomic<std::uint64_t> backwardJumps_{0};
	std::vector<Taken> taken_;
	std::atomic<std::size_t> takenCount_{0};
};

/// D: records its cycles, the B takenAt it sees, and what each cycle's samples were.
class Downstream final : public ProbeCode {
public:
	enum class Heard : std::uint8_t { zeros, ramp, other };

	Downstream(Shared& shared, const TakeUp& block)
		: shared_(shared), block_(block), log(kS2Capacity), takenAtSeen(kS2Capacity), heard(kS2Capacity),
		  firstWrong(kS2Capacity), frames(kS2Capacity) {}
	void cycle(const spa_io_clock& clock, std::span<const float* const> inputs, std::span<float* const>,
	           std::uint32_t count) noexcept override {
		const std::uint64_t sequence = shared_.sequence.fetch_add(1, std::memory_order_acq_rel);
		const std::uint64_t seenTakenAt = block_.takenAt();
		if (!shared_.recording.load(std::memory_order_acquire)) return;
		const auto n = log.add(clock.position, sequence);
		if (!n) return;
		const float* in = inputs[0];
		bool zeros = true;
		bool ramp = true;
		std::uint32_t wrong = count;
		for (std::uint32_t i = 0; i < count; ++i) {
			if (in[i] != 0.0F) zeros = false;
			if (in[i] != rampAt(clock.position + i)) {
				ramp = false;
				if (wrong == count && in[i] != 0.0F) wrong = i;
			}
		}
		takenAtSeen[*n] = seenTakenAt;
		heard[*n] = zeros ? Heard::zeros : ramp ? Heard::ramp : Heard::other;
		firstWrong[*n] = wrong;
		frames[*n] = count;
		log.commit(*n);
	}

private:
	Shared& shared_;
	const TakeUp& block_;

public:
	CycleLog log;
	std::vector<std::uint64_t> takenAtSeen;
	std::vector<Heard> heard;
	std::vector<std::uint32_t> firstWrong; // with Heard::other: the first sample neither zero nor the ramp
	std::vector<std::uint32_t> frames;
};

/// The three logs read as one timeline of graph cycles. U, B and D all run on the one data thread and each takes the
/// shared entry sequence number on entry, so a graph cycle is a run of consecutive numbers: U's, then B's, then D's, all
/// at one clock.position. Cycles are told apart by sequence, never by position: with the driver's default resync.ms
/// positions repeated and stepped back, and the check that they no longer do must not rest on them.
struct Timeline {
	std::size_t cycles = 0;        // U's entries: the cycles the driver started while recording
	std::size_t whole = 0;         // cycles all three ran
	std::size_t onlyU = 0;         // cycles cut short after U
	std::size_t onlyUB = 0;        // cycles cut short after B
	std::size_t broken = 0;        // B or D entries not straight after their upstream's, in its cycle, at its position
	std::string firstBroken;       // the first of those, described
	std::size_t stepsBack = 0;     // cycles whose position is not above the previous cycle's
	std::uint64_t largestBack = 0; // frames, the largest step back
	std::size_t tooFarAhead = 0;   // cycles more than 10 ms ahead of the raw clock, where the driver puts none
	std::string firstTooFarAhead;  // the first of those, described
	/// Every cycle U ran while recording, in the order they ran: its position and the index of U's, B's and D's entry
	/// in their logs, kNone where that node did not run it.
	struct Run {
		static constexpr std::size_t kNone = SIZE_MAX;
		std::uint64_t position;
		std::size_t u = kNone;
		std::size_t b = kNone;
		std::size_t d = kNone;
	};
	std::vector<Run> runs;
};

Timeline readTimeline(const Upstream& u, const CycleLog& b, const CycleLog& d) {
	struct Entry {
		std::uint64_t sequence;
		std::uint8_t which; // 0 U, 1 B, 2 D
		std::size_t index;
		std::uint64_t position;
	};
	std::vector<Entry> entries;
	const std::array<const CycleLog*, 3> logs{&u.log, &b, &d};
	for (std::uint8_t which = 0; which < logs.size(); ++which) {
		const std::size_t n = logs[which]->count.load();
		for (std::size_t i = 0; i < n; ++i)
			entries.push_back(Entry{logs[which]->entry[i], which, i, logs[which]->position[i]});
	}
	std::sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) { return x.sequence < y.sequence; });

	Timeline result;
	std::optional<Entry> cycleU, cycleB;
	bool cycleD = false;
	const auto closeCycle = [&] {
		if (!cycleU) return;
		if (cycleD) ++result.whole;
		else if (cycleB) ++result.onlyUB;
		else ++result.onlyU;
	};
	const auto broke = [&](const Entry& e, const char* why) {
		if (result.broken++ == 0)
			result.firstBroken = std::string(e.which == 1 ? "B" : "D") + " entry " + std::to_string(e.sequence) +
			                     " at " + std::to_string(e.position) + ": " + why;
	};
	for (const Entry& e : entries) {
		if (e.which == 0) {
			if (cycleU && e.position <= cycleU->position) {
				++result.stepsBack;
				result.largestBack = std::max(result.largestBack, cycleU->position - e.position);
			}
			// The driver's loop holds clock.position on the raw clock (as it reads it, before U enters), and a stall
			// only puts it behind: no cycle starts 10 ms ahead of it.
			if (e.position > u.rawAtEntry[e.index] + kResyncFrames && result.tooFarAhead++ == 0)
				result.firstTooFarAhead =
					std::to_string(e.position) + " with the raw clock at " + std::to_string(u.rawAtEntry[e.index]);
			closeCycle();
			++result.cycles;
			result.runs.push_back(Timeline::Run{e.position, e.index});
			cycleU = e;
			cycleB.reset();
			cycleD = false;
		} else if (!cycleU) {
			continue; // recording began inside a cycle, after U's entry
		} else if (e.which == 1) {
			if (cycleB) broke(e, "B twice in one cycle");
			else if (e.sequence != cycleU->sequence + 1) broke(e, "not straight after U");
			else if (e.position != cycleU->position) broke(e, "not at U's position");
			cycleB = e;
			result.runs.back().b = e.index;
		} else {
			if (!cycleB) broke(e, "D without B in its cycle");
			else if (cycleD) broke(e, "D twice in one cycle");
			else if (e.sequence != cycleB->sequence + 1) broke(e, "not straight after B");
			else if (e.position != cycleB->position) broke(e, "not at B's position");
			cycleD = true;
			if (cycleB) result.runs.back().d = e.index;
		}
	}
	closeCycle();
	return result;
}

/// What S2's storm counts. Declared before the engine, so a storm invoke still queued when a test leaves never touches a
/// freed counter.
struct StormCounts {
	bool drained() const { return ran.load() == posts.load(); }
	std::atomic<std::uint64_t> posts{0};
	std::atomic<std::uint64_t> ran{0};
	std::atomic<std::uint64_t> refused{0};
};

/// The storm: no-op invokes into the data loop from a second thread, for as long as it lives, as fast as the data loop
/// takes them: bursts of 100, at most kBacklog outstanding, with a 50 us pause after each burst. Unpaced ("as fast as it
/// can"), one thread wedges PipeWire 1.4.2's data loop for good: loop.c's flush_all_queues runs until every queue is
/// empty and scans every overflow queue for each item, so once the poster gets ahead the data loop only flushes, never
/// processes again, and the overflow queues grow without bound (the first run of S2). Declared after the engine, so it
/// stops posting before the loop goes.
class InvokeStorm {
public:
	InvokeStorm(pw_loop* dataLoop, StormCounts& counts)
		: thread_([this, dataLoop, &counts] {
			  const auto noop = [](spa_loop*, bool, std::uint32_t, const void*, std::size_t, void* userData) {
				  static_cast<std::atomic<std::uint64_t>*>(userData)->fetch_add(1, std::memory_order_relaxed);
				  return 0;
			  };
			  while (on_.load(std::memory_order_relaxed)) {
				  for (int i = 0; i < 100; ++i) {
					  if (counts.posts.load(std::memory_order_relaxed) - counts.ran.load(std::memory_order_relaxed) >=
				          kBacklog)
						  break;
					  if (pw_loop_invoke(dataLoop, noop, SPA_ID_INVALID, nullptr, 0, false, &counts.ran) < 0)
						  counts.refused.fetch_add(1, std::memory_order_relaxed);
					  counts.posts.fetch_add(1, std::memory_order_relaxed);
				  }
				  const timespec pause{0, 50'000};
				  nanosleep(&pause, nullptr);
			  }
		  }) {}
	~InvokeStorm() { stop(); }
	InvokeStorm(const InvokeStorm&) = delete;
	InvokeStorm& operator=(const InvokeStorm&) = delete;
	void stop() {
		on_.store(false);
		if (thread_.joinable()) thread_.join();
	}

private:
	static constexpr std::uint64_t kBacklog =
		2000; // under one queue's worth of overflow chains: about 4 queues of 32 KiB
	std::atomic<bool> on_{true};
	std::thread thread_; // last: it starts once on_ is made
};

// ---------------------------------------------------------------------------------------------------------------------
// S5

/// S5's B keeps time as a source would with sendspin's feed at 48 kHz (feedFramesFor: 40 ms rounded up to a power of
/// two): a forward jump of at most this many frames is skipped and counted played; a larger one is a new clock.
constexpr std::uint32_t kS5Capacity = 2048;
/// The cycles S5 cuts short itself, of each shape: U overruns every kS5OverrunEvery-th of its cycles (cut after U), and
/// B as many of its own, half a period out of step (cut after B), until each has overrun kS5Overruns. A host cuts cycles
/// short on its own only by chance (S2: 1 to 17 in 25 s idle; one GitHub runner 9 in 60 s beside a busy thread per core,
/// where a loaded workstation cut 5,000), so S5 never waits for those: they are counted with the rest when they come.
constexpr std::size_t kS5Overruns = 200;
constexpr std::size_t kS5OverrunEvery = 16;
/// About 4.5 s at 750 cycles a second; the deadline only fails the test.
constexpr std::chrono::seconds kS5Deadline{30};
/// Cycles a log holds: the driver starts 750 a second at 64 frames, and its catch-up after a stall only makes up the
/// ones it missed.
constexpr std::size_t kS5Log = 750 * 40;

/// B: a test-only source served by its block. Before its code (which here does nothing more) it plays a ring of
/// kS5Capacity frames into its output and keeps time as slice 3b's source will: a cycle whose position
/// jumped ahead of the last one's end by at most the capacity skips that many frames and counts them played; any other
/// jump is a new clock, played on from where the ring is (so `total - (position - first position)` takes a new value
/// there, the anchor S5's model re-takes). `total` counts frames played and skipped, as a source's totalFrames. The
/// ring's frame k is rampAt(k), and the writer side is not under test: the ring is kept full, each frame consumed
/// replaced by the one kS5Capacity later. It records, per cycle, the stamp of what it read from U and whether it took a
/// new clock; it publishes the position and first ring frame of the cycle it plays, for D in the same cycle; and it
/// overruns `overruns` of its recorded cycles once it has played.
class RingSource final : public ProbeCode {
public:
	RingSource(Shared& shared, Waiter& waiter, StallPlan overruns)
		: shared_(shared), waiter_(waiter), overruns_(std::move(overruns)), ring_(kS5Capacity), log(kS5Log),
		  totalBefore(kS5Log), stamp(kS5Log), uniform(kS5Log), overran(kS5Log), newClock(kS5Log) {
		for (std::uint32_t k = 0; k < kS5Capacity; ++k) ring_[k] = rampAt(k);
	}
	std::uint64_t firstPosition() const { return firstPosition_.load(std::memory_order_acquire); }
	std::uint64_t cycles() const { return cycles_.load(std::memory_order_acquire); }
	std::uint64_t newClocks() const { return newClocks_.load(std::memory_order_acquire); }
	/// The data thread, in the cycle B played (D runs after it in that cycle, on the same thread): its position and the
	/// index in the ring's ramp of the first frame it played.
	std::uint64_t playedAt() const { return playedAt_.load(std::memory_order_relaxed); }
	std::uint64_t playedFrom() const { return playedFrom_.load(std::memory_order_relaxed); }

	void cycle(const spa_io_clock& clock, std::span<const float* const> inputs, std::span<float* const> outputs,
	           std::uint32_t frames) noexcept override {
		const std::uint64_t sequence = shared_.sequence.fetch_add(1, std::memory_order_acq_rel);
		const std::uint64_t at = clock.position;
		const bool recording = shared_.recording.load(std::memory_order_acquire);
		if (!started_ && !recording) {
			// Bound once recording begins, as a drive binds a source: so its first played frame is the first recorded.
			std::fill_n(outputs[0], frames, 0.0F);
			cycles_.fetch_add(1, std::memory_order_release);
			waiter_.wake();
			return;
		}
		bool newClockHere = false; // this cycle took a new clock
		if (!started_) {
			started_ = true;
			firstPosition_.store(at, std::memory_order_relaxed);
			started.store(true, std::memory_order_release);
		} else if (at != next_) {
			if (at > next_ && at - next_ <= kS5Capacity) consume(at - next_);
			else newClockHere = true;
		}
		if (newClockHere) newClocks_.fetch_add(1, std::memory_order_relaxed);
		next_ = at + frames;
		const std::uint64_t before = total_;
		playedAt_.store(at, std::memory_order_relaxed);
		playedFrom_.store(before, std::memory_order_relaxed);
		if (frames <= kS5Capacity) {
			for (std::uint32_t i = 0; i < frames; ++i) outputs[0][i] = ring_[(total_ + i) & (kS5Capacity - 1)];
			consume(frames);
		}
		const float* in = inputs[0];
		bool same = true;
		for (std::uint32_t i = 1; i < frames; ++i) same = same && in[i] == in[0];
		if (recording) {
			if (const auto n = log.add(at, sequence)) {
				totalBefore[*n] = before;
				stamp[*n] = in[0];
				uniform[*n] = same ? 1 : 0;
				overran[*n] = overruns_.plan().planned(*n) ? 1 : 0;
				newClock[*n] = newClockHere ? 1 : 0;
				log.commit(*n);
				overruns_(*n, clock.duration, kRate);
			}
		}
		cycles_.fetch_add(1, std::memory_order_release);
		waiter_.wake();
	}

private:
	/// Frames played or skipped: each slot freed takes the frame kS5Capacity later, so the ring stays full.
	void consume(std::uint64_t frames) noexcept {
		for (std::uint64_t k = total_; k < total_ + frames; ++k) ring_[k & (kS5Capacity - 1)] = rampAt(k + kS5Capacity);
		total_ += frames;
	}

	Shared& shared_;
	Waiter& waiter_;
	Stalls overruns_;
	// Data thread only.
	std::vector<float> ring_;
	std::uint64_t total_ = 0;
	std::uint64_t next_ = 0;
	bool started_ = false;
	// Read elsewhere.
	std::atomic<std::uint64_t> firstPosition_{0};
	std::atomic<std::uint64_t> cycles_{0};
	std::atomic<std::uint64_t> newClocks_{0};
	std::atomic<std::uint64_t> playedAt_{0};
	std::atomic<std::uint64_t> playedFrom_{0};

public:
	std::atomic<bool> started{false}; // bound: firstPosition() holds
	CycleLog log;
	std::vector<std::uint64_t> totalBefore; // per cycle: `total` before it played, the index of its first frame
	std::vector<float> stamp;               // per cycle: the first frame read from U
	std::vector<std::uint8_t> uniform;      // per cycle: every frame read from U was that one
	std::vector<std::uint8_t> overran;      // per cycle: B overran it
	std::vector<std::uint8_t> newClock;     // per cycle: B took a new clock (its position jumped beyond the capacity)
	std::size_t overrunCount() const { return overruns_.made(); }
};

/// S5's D: per cycle, whether every frame is the ring frame B played at that position in the same cycle, rampAt(B's
/// first ring frame of it + i). That B's ring frames keep to the clock (between new clocks) S5 judges from B's own log.
class RampListener final : public ProbeCode {
public:
	RampListener(Shared& shared, const RingSource& source)
		: shared_(shared), source_(source), log(kS5Log), exact(kS5Log), firstWrong(kS5Log) {}
	void cycle(const spa_io_clock& clock, std::span<const float* const> inputs, std::span<float* const>,
	           std::uint32_t frames) noexcept override {
		const std::uint64_t sequence = shared_.sequence.fetch_add(1, std::memory_order_acq_rel);
		if (!shared_.recording.load(std::memory_order_acquire)) return;
		if (!source_.started.load(std::memory_order_acquire)) return; // a cycle before B's first recorded one
		const auto n = log.add(clock.position, sequence);
		if (!n) return;
		// B ran this cycle before D (D runs only in cycles B ran), so it published this position's frames.
		const bool atB = source_.playedAt() == clock.position;
		const std::uint64_t index = source_.playedFrom();
		std::uint32_t wrong = atB ? frames : 0;
		for (std::uint32_t i = 0; i < frames && wrong == frames; ++i) {
			if (inputs[0][i] != rampAt(index + i)) wrong = i;
		}
		exact[*n] = wrong == frames ? 1 : 0;
		firstWrong[*n] = wrong;
		log.commit(*n);
	}

private:
	Shared& shared_;
	const RingSource& source_;

public:
	CycleLog log;
	std::vector<std::uint8_t> exact;
	std::vector<std::uint32_t> firstWrong;
};

/// One thread per core spinning at normal priority for as long as it lives: S5's load of its own.
class BusyCores {
public:
	BusyCores() {
		const unsigned cores = std::max(1U, std::thread::hardware_concurrency());
		for (unsigned i = 0; i < cores; ++i) {
			threads_.emplace_back([this] {
				std::uint64_t spin = 0;
				while (on_.load(std::memory_order_relaxed)) spin = spin * 6364136223846793005ULL + 1;
				sink_.fetch_add(spin, std::memory_order_relaxed);
			});
		}
	}
	~BusyCores() {
		on_.store(false);
		for (std::thread& thread : threads_) thread.join();
	}
	BusyCores(const BusyCores&) = delete;
	BusyCores& operator=(const BusyCores&) = delete;
	std::size_t count() const { return threads_.size(); }

private:
	std::atomic<bool> on_{true};
	std::atomic<std::uint64_t> sink_{0};
	std::vector<std::thread> threads_;
};

// ---------------------------------------------------------------------------------------------------------------------
// S3

/// Threads of this process whose name starts with `prefix` (/proc/self/task/<tid>/comm).
std::vector<std::string> threadsNamed(const std::string& prefix) {
	std::vector<std::string> names;
	for (const auto& task : std::filesystem::directory_iterator("/proc/self/task")) {
		std::ifstream comm(task.path() / "comm");
		std::string name;
		std::getline(comm, name);
		if (name.starts_with(prefix)) names.push_back(name);
	}
	return names;
}

class CompanionEvents final : public EngineCompanionEvents {
public:
	explicit CompanionEvents(Waiter& waiter) : waiter_(waiter) {}
	void ready() override {
		isReady = true;
		waiter_.wake();
	}
	void lost(std::string detail) override {
		lostDetail = std::move(detail);
		waiter_.wake();
	}
	bool isReady = false;
	std::optional<std::string> lostDetail;

private:
	Waiter& waiter_;
};

class Silence final : public AudioCode {
public:
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
	}
	void release() noexcept override {}
};

class Counting final : public ProbeCode {
public:
	void cycle(const spa_io_clock&, std::span<const float* const>, std::span<float* const> outputs,
	           std::uint32_t frames) noexcept override {
		for (float* out : outputs) std::fill_n(out, frames, 0.0F);
		cycles.fetch_add(1, std::memory_order_release);
	}
	std::atomic<std::uint64_t> cycles{0};
};

} // namespace

TEST_CASE(
	"S1: invokes posted from the main loop without blocking run on the data thread between cycles, in order, "
	"each completion heard through a main-loop event, allocating nothing there, and a stalled data loop never makes "
	"a post wait or fail",
	"[spike][s1]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	NoBell bell;
	FilterPortBlocks blocks(*engine, bell);
	InvokeRecord record(waiter, engine->mainLoop());
	FlaggedCode codeA(record);
	StallingCode codeB(record);
	FlaggedProbe probeCode(record);
	BlockEvents eventsA(waiter), eventsB(waiter);
	auto a = blocks.make("a", Ports{{{Direction::output, "out", {"1"}}}}, kFormat, eventsA);
	auto b =
		blocks.make("b", Ports{{{Direction::input, "in", {"1"}}, {Direction::output, "out", {"1"}}}}, kFormat, eventsB);
	REQUIRE(ear::testing::driveNow(*a, &codeA));
	REQUIRE(ear::testing::driveNow(*b, &codeB));
	Probe probe(*engine, waiter, "probe", 0, 1, probeCode);
	REQUIRE(waiter.until([&] {
		return eventsA.isReady && eventsB.isReady && probe.streaming() &&
		       record.cycles.load(std::memory_order_relaxed) > 30;
	}));
	pw_loop* dataLoop = contextDataLoop(*engine);
	REQUIRE(probe.dataLoop() == dataLoop); // the blocks' loop is the context's one data loop (S3)
	REQUIRE(record.dataThreadKnown.load());

	Poster poster(record, dataLoop, engine->mainLoop());
	std::vector<double> burstMs;
	constexpr std::uint64_t kPhase = 1000;
	for (std::uint64_t phase = 0; phase < InvokeRecord::kNormal / kPhase; ++phase) {
		const std::uint64_t first = phase * kPhase + 1, last = (phase + 1) * kPhase;
		if (phase % 2 == 0) {
			burstMs.push_back(poster.burst(first, last));
		} else {
			poster.singly(first, last);
		}
		REQUIRE(waiter.until([&] { return record.seen >= last; }, 10s));
	}

	// The stall: block b's process sleeps 2 s while the main thread keeps posting, 3,000 invokes 200 us apart (about
	// 0.6 s, inside the stall): far more than one invoke queue holds (32 KiB, spa/plugins/support/loop.c), so it fills.
	codeB.armed.store(true);
	REQUIRE(waiter.until([&] { return codeB.stalling.load(); }));
	const std::size_t ranBeforeStall = record.ran.load();
	const std::uint64_t allocationsBefore = countedAllocations();
	countAllocationsOnThisThread(true); // the main thread's own, while the data thread sleeps
	double slowestStalledMs = 0.0;
	std::size_t stalledOverOneMs = 0;
	for (std::uint64_t number = InvokeRecord::kNormal + 1; number <= InvokeRecord::kCount; ++number) {
		const double ms = poster.one(number);
		slowestStalledMs = std::max(slowestStalledMs, ms);
		if (ms >= 1.0) ++stalledOverOneMs;
		const timespec pause{0, 200'000};
		nanosleep(&pause, nullptr);
	}
	countAllocationsOnThisThread(false);
	const bool stillStalled = codeB.stalling.load();
	const std::uint64_t mainAllocations = countedAllocations() - allocationsBefore;
	const std::size_t ranDuringStall = record.ran.load() - ranBeforeStall;
	REQUIRE(waiter.until([&] { return record.seen >= InvokeRecord::kCount; }, 10s));
	CHECK(codeB.stalled.load());
	REQUIRE(ear::testing::driveNow(*a, nullptr));
	REQUIRE(ear::testing::driveNow(*b, nullptr));

	const std::size_t ran = record.ran.load();
	const auto results = poster.results();
	std::size_t refused = 0, notAsync = 0, outOfOrder = 0, offThread = 0, insideProcess = 0, toldSync = 0;
	for (std::size_t i = 0; i < InvokeRecord::kCount; ++i) {
		if (results[i] < 0) ++refused;
		if (results[i] != static_cast<int>(SPA_RESULT_RETURN_ASYNC(static_cast<std::uint32_t>(i + 1)))) ++notAsync;
		if (record.order[i] != i + 1) ++outOfOrder;
		if (record.onDataThread[i] == 0) ++offThread;
		if (record.duringProcess[i] != 0) ++insideProcess;
		if (record.async[i] == 0) ++toldSync;
	}
	const std::uint64_t allocations = countedAllocations() - mainAllocations; // the data thread's
	const double worstBurst = *std::ranges::max_element(burstMs);
	std::string bursts;
	for (double ms : burstMs) bursts += std::to_string(ms) + " ";
	std::printf(
		"S1: %zu invokes ran; bursts of 1000 cost the main loop %sms (worst %.3f); slowest single post %.3f ms; "
		"%" PRIu64 " completion events for %" PRIu64 " completions; %" PRIu64
		" functions had already run when their post returned; %" PRIu64 " allocations on the data thread\n",
		ran, bursts.c_str(), worstBurst, poster.slowestSingleMs(), record.completionEvents, record.seen,
		poster.ranBeforeReturn(), allocations);
	INFO("bursts (ms): " << bursts);
	CHECK(ran == InvokeRecord::kCount);
	CHECK(refused == 0);
	CHECK(notAsync == 0); // never the function's own result: no post waited for its function or ran it inline
	CHECK(outOfOrder == 0);
	CHECK(offThread == 0);
	CHECK(insideProcess == 0);
	CHECK(toldSync == 0);
	CHECK(record.seen == InvokeRecord::kCount); // every completion reached the main loop, though signals coalesced
	CHECK(record.completionEvents <= InvokeRecord::kCount);
	CHECK(allocations == 0);
	CHECK(worstBurst < 5.0);
	// The stall. PipeWire 1.4.2 never refuses or waits on a full queue: the posting thread allocates an overflow queue
	// (loop.c, loop_queue_invoke's xrun path) and carries on; the data thread runs everything, in order, once it wakes.
	std::printf(
		"S1 stall: %" PRIu64 " posts during a 2 s stall (still stalled after them: %s), slowest %.3f ms, %zu of "
		"them 1 ms or more, %zu functions ran meanwhile, %" PRIu64 " allocations on the posting (main) thread\n",
		InvokeRecord::kStalled, stillStalled ? "yes" : "no", slowestStalledMs, stalledOverOneMs, ranDuringStall,
		mainAllocations);
	CHECK(stillStalled);
	CHECK(ranDuringStall == 0);
	// A post costs microseconds; one past 1 ms is the main thread preempted on a loaded host, never a wait on the
	// stalled loop, which would cost the rest of the 2 s. So a few are let through, and none near the stall.
	CHECK(stalledOverOneMs <= InvokeRecord::kStalled / 1000);
	CHECK(slowestStalledMs < 50.0);
}

TEST_CASE(
	"S1: on a data loop whose thread is not running (not yet started, or stopped), a non-blocking invoke runs its "
	"function inline on the caller and returns its result",
	"[spike][s1]") {
	auto engine = makeEngine(); // pw_init
	pw_data_loop* dataLoop = pw_data_loop_new(nullptr);
	REQUIRE(dataLoop != nullptr);
	struct Seen {
		std::atomic<int> calls{0};
		std::atomic<bool> onCaller{false};
		pthread_t caller = pthread_self();
	} seen;
	const auto function = [](spa_loop*, bool, std::uint32_t, const void*, std::size_t, void* userData) {
		auto* self = static_cast<Seen*>(userData);
		self->onCaller.store(pthread_equal(pthread_self(), self->caller) != 0);
		self->calls.fetch_add(1);
		return InvokeRecord::kFunctionResult;
	};
	pw_loop* loop = pw_data_loop_get_loop(dataLoop);
	// Not started: inline, the function's own result.
	CHECK(pw_loop_invoke(loop, function, 1, nullptr, 0, false, &seen) == InvokeRecord::kFunctionResult);
	CHECK(seen.calls.load() == 1);
	CHECK(seen.onCaller.load());
	// Started: inline until its thread enters the loop (the race Engine::create waits out), then on that thread.
	REQUIRE(pw_data_loop_start(dataLoop) >= 0);
	int inlineAfterStart = 0;
	int result = 0;
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (std::chrono::steady_clock::now() < deadline) {
		const int before = seen.calls.load();
		result = pw_loop_invoke(loop, function, 2, nullptr, 0, false, &seen);
		while (seen.calls.load() == before && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		if (!seen.onCaller.load()) break;
		++inlineAfterStart;
	}
	CHECK_FALSE(seen.onCaller.load());
	CHECK(result == static_cast<int>(SPA_RESULT_RETURN_ASYNC(2)));
	// Stopped: inline again (loop.c's loop_leave clears the loop's thread).
	REQUIRE(pw_data_loop_stop(dataLoop) >= 0);
	const int calls = seen.calls.load();
	CHECK(pw_loop_invoke(loop, function, 3, nullptr, 0, false, &seen) == InvokeRecord::kFunctionResult);
	CHECK(seen.calls.load() == calls + 1);
	CHECK(seen.onCaller.load());
	std::printf("S1 no thread: inline before start and after stop; %d inline invokes after start before the thread "
	            "entered its loop\n",
	            inlineAfterStart);
	pw_data_loop_destroy(dataLoop);
}

TEST_CASE(
	"S2: under an invoke storm a block and its peers see one position per cycle in link order, an xrun cut "
	"short only by a prefix of the graph, and a ramp taken up by position arrives sample-exact across 1000 requests",
	"[spike][s2]") {
	// Declared before the engine, so a storm invoke still queued when the test leaves never touches a freed counter.
	StormCounts stormCounts;
	auto engine = engineAt(kS2Block);
	Waiter waiter(*engine);
	Wiring wiring(*engine, waiter);
	Shared shared;
	Upstream upCode(shared);
	TakeUp blockCode(shared, waiter);
	Downstream downCode(shared, blockCode);
	Probe up(*engine, waiter, "u", 0, 1, upCode);
	Probe block(*engine, waiter, "b", 1, 1, blockCode);
	Probe down(*engine, waiter, "d", 1, 0, downCode);
	REQUIRE(waiter.until([&] { return up.streaming() && block.streaming() && down.streaming(); }));
	REQUIRE(wiring.link(waiter, {"u", "out.1"}, {"b", "in.1"}));
	REQUIRE(wiring.link(waiter, {"b", "out.1"}, {"d", "in.1"}));
	pw_loop* dataLoop = contextDataLoop(*engine);
	REQUIRE(block.dataLoop() == dataLoop);

	InvokeStorm storm(dataLoop, stormCounts);

	shared.recording.store(true);
	const std::uint64_t armed = blockCode.cycles();
	REQUIRE(waiter.until([&] { return blockCode.cycles() > armed + 4; }));

	// Drive and silence alternately: each request once the previous one completed, then 1 to 30 cycles later.
	const auto storm0 = std::chrono::steady_clock::now();
	std::mt19937 random(20261108);
	std::uniform_int_distribution<std::uint64_t> interval(1, 30);
	std::uint64_t posted = 0;
	std::uint64_t waitUntil = 0;
	bool waitingForDone = false;
	std::size_t refusedPosts = 0;
	const auto postNext = [&] {
		++posted;
		if (blockCode.post(dataLoop, TakeUp::Request{posted, posted % 2 == 1}) < 0) ++refusedPosts;
		waitingForDone = true;
	};
	postNext();
	const bool finished = waiter.until(
		[&] {
			if (waitingForDone) {
				if (blockCode.applied() != posted) return false;
				waitingForDone = false;
				waitUntil = blockCode.cycles() + interval(random);
			}
			if (blockCode.cycles() < waitUntil) return false;
			if (posted == kS2Requests) return true;
			postNext();
			return false;
		},
		std::chrono::seconds(kS2Requests * 40 * kS2Block / kRate * 3 + 30));
	const double seconds = millisecondsSince(storm0) / 1000.0;
	// Two cycles more, so every log holds the last request's cycles, then stop recording and the storm.
	const std::uint64_t last = blockCode.cycles();
	REQUIRE(waiter.until([&] { return blockCode.cycles() > last + 2; }));
	shared.recording.store(false);
	storm.stop();
	REQUIRE(finished);
	REQUIRE(waiter.until([&] { return stormCounts.drained(); }));

	// Every log complete: a cycle dropped from a full log would void what follows.
	REQUIRE_FALSE(upCode.log.full.load());
	REQUIRE_FALSE(blockCode.log.full.load());
	REQUIRE_FALSE(downCode.log.full.load());

	// Cycle by cycle (by entry sequence, never by position): U, B and D see one clock.position in each cycle, entered in
	// link order. A cycle the data thread cannot finish before the driver starts the next (the graph's xrun: a loaded
	// host) is abandoned part way: only a prefix of the graph in link order runs it (U, or U and B) and its audio never
	// reaches the rest. So such a cycle is counted and printed, and fails only if a node ran it without every node
	// upstream of it straight before, at the same position. Positions never step back: a starved data loop reads the
	// driver's timer late, and the driver, which engine.conf's resync.ms keeps from resetting to the raw clock, fires the
	// timeouts it missed back to back, a block each, until it has caught up (node-driver.c on_timeout). With the default
	// resync.ms (10 ms) it reset the position forward to the raw clock, then stepped it back by up to about 440 frames
	// once its back-to-back timeouts had put it 10 ms ahead, and the graph ran positions again. No cycle starts more than
	// 10 ms ahead of the raw clock either.
	const Timeline timeline = readTimeline(upCode, blockCode.log, downCode.log);
	INFO("first broken cycle: " << timeline.firstBroken);
	INFO("first cycle too far ahead of the raw clock: " << timeline.firstTooFarAhead);
	CHECK(timeline.whole > 0);
	CHECK(timeline.broken == 0);
	CHECK(timeline.tooFarAhead == 0);
	INFO("largest step back: " << timeline.largestBack << " frames");
	CHECK(timeline.stepsBack == 0);

	// Every request taken up once, in order, alternating; takenAt published one cycle before.
	const auto taken = blockCode.taken();
	REQUIRE(taken.size() == kS2Requests);
	std::size_t wrongOrder = 0, notOneCycle = 0, late = 0;
	for (std::size_t i = 0; i < taken.size(); ++i) {
		if (taken[i].number != i + 1 || taken[i].drive != (i % 2 == 0)) ++wrongOrder;
		if (taken[i].takenAt != taken[i].publishedAt + kS2Block) ++notOneCycle;
		if (taken[i].swappedAt != taken[i].takenAt) ++late;
	}
	CHECK(wrongOrder == 0);
	CHECK(notOneCycle == 0);
	CHECK(blockCode.overlaps() == 0);

	// D, cycle by cycle in the order the cycles ran: the ramp from exactly the cycle of each drive's swap to exactly the
	// cycle before the next silence's swap, zeros elsewhere: no gap, no trailing zero. A swap is in force from B's cycle
	// that made it, so for D from the cycle whose entry follows that one; comparing positions instead would put a swap
	// made after a step back in force in cycles that ran before it. And D saw takenAt in the very cycle B published it
	// (when D ran that cycle).
	const std::size_t nd = downCode.log.count.load();
	std::size_t wrongCycles = 0, otherCycles = 0, rampCycles = 0, wrongFrames = 0, missedPublish = 0, publishSeen = 0;
	std::string firstWrong;
	std::size_t next = 0; // the first taken entry not yet in force
	bool driving = false;
	for (std::size_t i = 0; i < nd; ++i) {
		const std::uint64_t at = downCode.log.position[i];
		const std::uint64_t sequence = downCode.log.entry[i];
		while (next < taken.size() && taken[next].swappedSeq < sequence) driving = taken[next++].drive;
		if (downCode.frames[i] != kS2Block) ++wrongFrames;
		const auto heard = downCode.heard[i];
		if (heard == Downstream::Heard::ramp) ++rampCycles;
		if (heard == Downstream::Heard::other) ++otherCycles;
		const auto wanted = driving ? Downstream::Heard::ramp : Downstream::Heard::zeros;
		if (heard != wanted) {
			if (wrongCycles == 0) {
				firstWrong = "D cycle " + std::to_string(i) + " at " + std::to_string(at) + ": heard " +
				             std::to_string(static_cast<int>(heard)) + ", wanted " +
				             std::to_string(static_cast<int>(wanted)) + ", first wrong sample " +
				             std::to_string(downCode.firstWrong[i]);
			}
			++wrongCycles;
		}
	}
	for (const auto& request : taken) {
		const auto first = downCode.log.entry.begin();
		const auto found = std::lower_bound(first, first + static_cast<std::ptrdiff_t>(nd), request.publishedSeq + 1);
		if (found == first + static_cast<std::ptrdiff_t>(nd) || *found != request.publishedSeq + 1) continue;
		++publishSeen;
		if (downCode.takenAtSeen[static_cast<std::size_t>(found - first)] != request.takenAt) ++missedPublish;
	}
	std::printf("S2: %zu requests taken up in %.1f s over %zu cycles of D (%zu with the ramp); %zu take-ups late by a "
	            "jump; B saw %" PRIu64 " position jumps (%" PRIu64 " backwards); %" PRIu64
	            " storm invokes (%.0f a second, %" PRIu64 " refused); %zu cycles started, %zu run by every node, %zu "
	            "cut short after U and %zu after B, %zu broken; %zu steps back (largest %" PRIu64
	            " frames; %zu cycles too far ahead of the raw clock); D saw %zu publications, %zu missed; %zu D cycles "
	            "wrong (%zu "
	            "neither zeros nor the ramp)\n",
	            taken.size(), seconds, nd, rampCycles, late, blockCode.jumps(), blockCode.backwardJumps(),
	            stormCounts.posts.load(), static_cast<double>(stormCounts.posts.load()) / seconds,
	            stormCounts.refused.load(), timeline.cycles, timeline.whole, timeline.onlyU, timeline.onlyUB,
	            timeline.broken, timeline.stepsBack, timeline.largestBack, timeline.tooFarAhead, publishSeen,
	            missedPublish, wrongCycles, otherCycles);
	INFO(firstWrong);
	CHECK(refusedPosts == 0);
	CHECK(stormCounts.refused.load() == 0);
	CHECK(stormCounts.posts.load() > 0);
	CHECK(wrongFrames == 0);
	CHECK(wrongCycles == 0);
	CHECK(missedPublish == 0);
	CHECK(rampCycles > 0);
}

TEST_CASE("S5: a source served by its block keeps its played count on the clock across cycles cut short after its "
          "upstream and after it, overrun on purpose under an invoke storm beside a busy thread per core",
          "[spike][s2][s5]") {
	// U → B → D at S2's block under its storm, beside a busy thread per core. U overruns kS5Overruns of its cycles and
	// B as many of its own, so each of those is cut short straight after the probe that overran it (overruns()), on any
	// host; the host's own cut-short cycles come with them, as many as it makes. Part A, the gate: across every cycle B
	// ran on one clock, `total - (position - first position)` stays what it was (no drift across a cut-short cycle),
	// and in every cycle B and D both ran D heard the ring frame B played at that position, sample for sample. A jump
	// beyond B's capacity is a new clock by the source's rule (docs/writing-a-node-type.md#sources): the planned
	// overruns make none (each cuts one or two cycles), but a host that stalls the data thread past the capacity (42 ms)
	// does, and there the model takes B's new anchor instead of counting a drift; such jumps are recorded, and every one
	// of them, and nothing else, must be a new clock to B. Part B, recorded only: in B's
	// first cycle after one U ran and B did not, whether B read U's frames of its own cycle ("current") or of the cut one
	// ("stale"); counted apart for cycles cut by an overrun and the rest.
	StormCounts stormCounts; // before the engine, as S2's
	auto engine = engineAt(kS2Block);
	Waiter waiter(*engine);
	Wiring wiring(*engine, waiter);
	Shared shared;
	Upstream upCode(shared, kS5Log, overruns(kS5OverrunEvery, kS5OverrunEvery / 4, kS5Overruns));
	RingSource sourceCode(shared, waiter, overruns(kS5OverrunEvery, kS5OverrunEvery * 3 / 4, kS5Overruns));
	RampListener downCode(shared, sourceCode);
	Probe up(*engine, waiter, "u", 0, 1, upCode);
	Probe block(*engine, waiter, "b", 1, 1, sourceCode);
	Probe down(*engine, waiter, "d", 1, 0, downCode);
	REQUIRE(waiter.until([&] { return up.streaming() && block.streaming() && down.streaming(); }));
	REQUIRE(wiring.link(waiter, {"u", "out.1"}, {"b", "in.1"}));
	REQUIRE(wiring.link(waiter, {"b", "out.1"}, {"d", "in.1"}));
	pw_loop* dataLoop = contextDataLoop(*engine);
	REQUIRE(block.dataLoop() == dataLoop);
	InvokeStorm storm(dataLoop, stormCounts);
	std::optional<BusyCores> busy;
	busy.emplace();
	const std::uint64_t settled = sourceCode.cycles();
	REQUIRE(waiter.until([&] { return sourceCode.cycles() > settled + 4; }));

	// Recording binds B's source and starts the overruns.
	shared.recording.store(true);
	const auto start = std::chrono::steady_clock::now();
	const bool overrunAll = waiter.until(
		[&] {
			return (upCode.overrunCount() >= kS5Overruns && sourceCode.overrunCount() >= kS5Overruns) ||
		           upCode.log.full.load();
		},
		kS5Deadline);
	const double seconds = millisecondsSince(start) / 1000.0;
	const std::size_t busyThreads = busy->count();
	const std::uint64_t last = sourceCode.cycles();
	REQUIRE(waiter.until([&] { return sourceCode.cycles() > last + 2; }));
	shared.recording.store(false);
	busy.reset();
	storm.stop();
	REQUIRE(waiter.until([&] { return stormCounts.drained(); }));
	INFO("overruns in " << seconds << " s: U " << upCode.overrunCount() << ", B " << sourceCode.overrunCount());
	REQUIRE(overrunAll);
	REQUIRE_FALSE(upCode.log.full.load());
	REQUIRE_FALSE(sourceCode.log.full.load());
	REQUIRE_FALSE(downCode.log.full.load());

	const Timeline timeline = readTimeline(upCode, sourceCode.log, downCode.log);
	INFO("first broken cycle: " << timeline.firstBroken);
	CHECK(timeline.broken == 0);

	// In the order the cycles ran (never by position: S2). Index 0 of a pair counts the host's own cut-short cycles
	// (and those the driver cut catching up after an overrun), index 1 the overrun ones.
	const std::uint64_t first = sourceCode.firstPosition();
	std::size_t uOverran = 0, uOverranCut = 0, bOverran = 0, bOverranCut = 0, otherAfterU = 0, otherAfterB = 0;
	std::size_t drifts = 0, bCycles = 0, both = 0, dWrong = 0;
	std::size_t jumps = 0, notWhole = 0, beyond = 0, back = 0, clocks = 0, clockNotBeyond = 0;
	std::array<std::size_t, 2> afterCut{}, driftsAfterCut{}, largestJumpAfterCut{};
	std::uint64_t largestJump = 0;
	std::string firstDrift, firstWrong;
	// Part B: what B read in its first cycle after a cut one, and in its cycles after one it ran.
	std::array<std::size_t, 2> current{}, stale{}, none{}, other{};
	std::size_t same = 0, mixed = 0;
	std::size_t steadyCurrent = 0, steadyPrevious = 0, steadyOther = 0, nextCurrent = 0, nextOther = 0;
	std::string firstOther;
	std::optional<std::int64_t> lastOffset;
	std::optional<std::uint64_t> lastEnd, previousRun;
	bool cutSinceB = false, lastWasAfterCut = false;
	std::size_t cutKind = 0; // of the latest cut-short cycle: 1 if U overran it
	std::uint64_t cutAt = 0;
	for (const Timeline::Run& run : timeline.runs) {
		const bool uOverrun = upCode.overran[run.u] != 0;
		if (uOverrun) ++uOverran;
		if (run.b == Timeline::Run::kNone) {
			if (uOverrun) ++uOverranCut;
			else ++otherAfterU;
			cutSinceB = true;
			cutKind = uOverrun ? 1 : 0;
			cutAt = run.position;
			previousRun = run.position;
			continue;
		}
		const bool bOverrun = sourceCode.overran[run.b] != 0;
		if (bOverrun) ++bOverran;
		if (run.d == Timeline::Run::kNone) {
			if (bOverrun) ++bOverranCut;
			else ++otherAfterB;
		}
		++bCycles;
		const std::uint64_t at = sourceCode.log.position[run.b];
		const auto offset =
			static_cast<std::int64_t>(sourceCode.totalBefore[run.b]) - static_cast<std::int64_t>(at - first);
		const bool jumpedBeyond = lastEnd && at > *lastEnd && at - *lastEnd > kS5Capacity;
		if (sourceCode.newClock[run.b] != 0) {
			// A new clock: the anchor is re-taken here, never a drift; it must come from a jump beyond the capacity.
			++clocks;
			if (!jumpedBeyond) ++clockNotBeyond;
		} else if (lastOffset && offset != *lastOffset) {
			if (drifts++ == 0)
				firstDrift = "B at " + std::to_string(at) + ": offset " + std::to_string(*lastOffset) + " to " +
				             std::to_string(offset) + (cutSinceB ? ", after a cut-short cycle" : "");
			if (cutSinceB) ++driftsAfterCut[cutKind];
		}
		lastOffset = offset;
		if (lastEnd && at != *lastEnd) {
			++jumps;
			if (at < *lastEnd) {
				++back;
			} else {
				largestJump = std::max(largestJump, at - *lastEnd);
				if (cutSinceB)
					largestJumpAfterCut[cutKind] = std::max<std::size_t>(largestJumpAfterCut[cutKind], at - *lastEnd);
				if ((at - *lastEnd) % kS2Block != 0) ++notWhole;
				if (at - *lastEnd > kS5Capacity) ++beyond;
			}
		}
		lastEnd = at + kS2Block;
		const float read = sourceCode.stamp[run.b];
		if (sourceCode.uniform[run.b] == 0) ++mixed;
		if (cutSinceB) {
			++afterCut[cutKind];
			if (rampAt(cutAt) == rampAt(at)) ++same; // a cut cycle at the position B runs: the two cannot be told apart
			else if (read == rampAt(at)) ++current[cutKind];
			else if (read == rampAt(cutAt)) ++stale[cutKind];
			else if (read == 0.0F) ++none[cutKind];
			else if (other[cutKind]++ == 0 && firstOther.empty())
				firstOther = "read " + std::to_string(read) + " at " + std::to_string(at) + " after a cut at " +
				             std::to_string(cutAt);
		} else if (read == rampAt(at)) {
			++steadyCurrent;
			if (lastWasAfterCut) ++nextCurrent;
		} else {
			if (previousRun && read == rampAt(*previousRun)) ++steadyPrevious;
			else ++steadyOther;
			if (lastWasAfterCut) ++nextOther;
		}
		lastWasAfterCut = cutSinceB;
		cutSinceB = false;
		previousRun = run.position;
		if (run.d != Timeline::Run::kNone) {
			++both;
			if (downCode.exact[run.d] == 0 && dWrong++ == 0)
				firstWrong = "D at " + std::to_string(downCode.log.position[run.d]) + ": first wrong frame " +
				             std::to_string(downCode.firstWrong[run.d]);
		}
	}
	std::printf(
		"S5 (%zu busy threads): %.1f s, %zu cycles started, %zu run by every node, %zu cut short after U (%zu of %zu U "
		"overran, %zu others) and %zu after B (%zu of %zu B overran, %zu others); %" PRIu64
		" storm invokes. A: B ran %zu cycles, %zu position jumps (largest %" PRIu64
		" frames: %zu after an overrun, %zu after another cut; %zu not whole cycles, %zu beyond its capacity of %u, "
		"%zu backwards; %" PRIu64 " new clocks, the model's anchor re-taken at each), %zu drifts of total - (position "
		"- first) (%zu after an overrun, %zu after another cut); D heard %zu cycles B ran too, %zu not the frame B "
		"played at that position. B: after %zu "
		"cycles U overran B read current %zu, stale %zu, none %zu, other %zu; after %zu other cut cycles current %zu, "
		"stale %zu, none %zu, other %zu; cut at its own position %zu; in its next cycle current %zu, other %zu; after "
		"cycles it ran, current %zu, the previous cycle's %zu, other %zu; %zu reads not one stamp\n",
		busyThreads, seconds, timeline.cycles, timeline.whole, timeline.onlyU, uOverranCut, uOverran, otherAfterU,
		timeline.onlyUB, bOverranCut, bOverran, otherAfterB, stormCounts.posts.load(), bCycles, jumps, largestJump,
		largestJumpAfterCut[1], largestJumpAfterCut[0], notWhole, beyond, kS5Capacity, back, sourceCode.newClocks(),
		drifts, driftsAfterCut[1], driftsAfterCut[0], both, dWrong, afterCut[1], current[1], stale[1], none[1],
		other[1], afterCut[0], current[0], stale[0], none[0], other[0], same, nextCurrent, nextOther, steadyCurrent,
		steadyPrevious, steadyOther, mixed);
	// Every overrun cut its cycle short straight after the probe that overran it, so each shape was met as planned.
	CHECK(uOverran == kS5Overruns);
	CHECK(bOverran == kS5Overruns);
	CHECK(uOverranCut == uOverran);
	CHECK(bOverranCut == bOverran);
	CHECK(timeline.onlyU >= kS5Overruns);
	CHECK(timeline.onlyUB >= kS5Overruns);
	// A, the gate.
	INFO(firstDrift);
	INFO(firstWrong);
	CHECK(stormCounts.refused.load() == 0);
	CHECK(both > 0);
	CHECK(drifts == 0);
	CHECK(notWhole == 0);
	// Each jump beyond the capacity, which only a host's stall makes, was a new clock to B, and B took no other.
	CHECK(clocks == beyond);
	CHECK(clockNotBeyond == 0);
	CHECK(back == 0);
	CHECK(dWrong == 0);
	INFO(firstOther);
	CHECK(mixed == 0);
}

TEST_CASE("S3: the engine has one data loop, context.num-data-loops = 1, and blocks, a sound card and an RTP receiver "
          "all run on it",
          "[spike][s3]") {
	ear::test::TempFolder alsaOut("card");
	setenv("ALSA_CONFIG_PATH", EAR_ALSA_TEST_DIR "/alsa.conf", 1);
	setenv("EAR_ALSA_OUT", alsaOut.path().c_str(), 1);
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Registry registry(*engine, waiter);
	NoBell bell;
	FilterPortBlocks blocks(*engine, bell);
	AlsaSoundCards cards(*engine, clocklessCardProperties());
	RtpReceivers receivers(*engine);
	Silence cardCode, fppCode;
	BlockEvents cardBlockEvents(waiter), fppBlockEvents(waiter);
	auto cardBlock = blocks.make("card",
	                             Ports{{{Direction::input, "play", {"1", "2"}},
	                                    {Direction::output, "to_card", {"1", "2"}, PortBank::Reach::inside}}},
	                             kFormat, cardBlockEvents);
	auto fppBlock = blocks.make("fpp",
	                            Ports{{{Direction::output, "stream", {"1", "2"}},
	                                   {Direction::input, "from_rtp", {"1", "2"}, PortBank::Reach::inside}}},
	                            kFormat, fppBlockEvents);
	REQUIRE(ear::testing::driveNow(*cardBlock, &cardCode));
	REQUIRE(ear::testing::driveNow(*fppBlock, &fppCode));
	Counting probeCode;
	Probe probe(*engine, waiter, "probe", 0, 1, probeCode);
	REQUIRE(waiter.until([&] { return cardBlockEvents.isReady && fppBlockEvents.isReady && probe.streaming(); }));

	CompanionEvents cardEvents(waiter), receiverEvents(waiter);
	auto card = cards.make(
		CompanionTarget{
			"card.playback", "card", Direction::output, {"to_card.1", "to_card.2"}, kFormat, cardBlock->blockId()},
		SoundCardShape{*CardId::parse("Test")}, cardEvents);
	auto receiver = receivers.make(
		CompanionTarget{"fpp.rtp", "fpp", Direction::input, {"from_rtp.1", "from_rtp.2"}, kFormat, fppBlock->blockId()},
		RtpReceiverShape{*RtpAddress::parse("0.0.0.0"), ear::test::freeUdpPort(), RtpEncoding::l24, kRate, 100},
		receiverEvents);
	const std::vector<NodeName> nodes{"Dummy-Driver", "card", "fpp", "probe", "card.playback", "fpp.rtp"};
	REQUIRE(waiter.until([&] {
		return (cardEvents.isReady || cardEvents.lostDetail) && (receiverEvents.isReady || receiverEvents.lostDetail) &&
		       std::ranges::all_of(nodes, [&](const NodeName& name) { return registry.props(name) != nullptr; });
	}));
	REQUIRE(cardEvents.lostDetail.value_or("") == "");
	REQUIRE(receiverEvents.lostDetail.value_or("") == "");
	REQUIRE(registry.reread(nodes)); // every node's whole current info

	const char* loops = pw_properties_get(pw_context_get_properties(engine->context()), "context.num-data-loops");
	const std::string theLoop = probe.dataLoop()->name;
	const auto dataThreads = threadsNamed("data-loop");
	std::string named, unnamed, other;
	for (const NodeName& name : nodes) {
		const auto& props = *registry.props(name);
		const auto found = props.find(PW_KEY_NODE_LOOP_NAME);
		if (found == props.end()) {
			unnamed += name + " ";
		} else if (found->second == theLoop) {
			named += name + " ";
		} else {
			other += name + "=" + found->second + " ";
		}
	}
	std::string threads;
	for (const std::string& thread : dataThreads) threads += thread + " ";
	std::printf("S3: context.num-data-loops = %s; the blocks' loop is %s (the context's: %s); data-loop threads: %s; "
	            "nodes naming it: %s; naming none: %s; naming another: %s\n",
	            loops != nullptr ? loops : "(unset)", theLoop.c_str(), contextDataLoop(*engine)->name, threads.c_str(),
	            named.c_str(), unnamed.c_str(), other.empty() ? "(none)" : other.c_str());
	CHECK(loops != nullptr);
	CHECK(std::string(loops != nullptr ? loops : "") == "1");
	CHECK(probe.dataLoop() == contextDataLoop(*engine));
	CHECK(dataThreads == std::vector<std::string>{theLoop}); // one data thread in the process, the blocks' loop's
	CHECK(other.empty());                                    // no node names any other loop
	CHECK(named.find("fpp.rtp") != std::string::npos);       // rtp-source names its loop itself
}
