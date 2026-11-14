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
//  - S5: a source served by its block, as ring companions are (docs/architecture.md#companions), on S2's harness
//    beside a busy thread per core, its upstream and itself overrunning chosen cycles so that both shapes of a
//    cut-short cycle occur on any host: its played count keeps to the clock across cycles an xrun cuts short, and the
//    node after it hears its frame of each position. And what a node reads after its upstream ran a cycle it did not:
//    the upstream's frames of the cycle it runs, never the cut one's (docs/engine.md#the-one-data-loop-and-invokes).
//  - S6: what the internal clock reports of every cycle (docs/engine.md#the-one-data-loop-and-invokes and
//    #the-internal-clock): every cycle a node misses is one it reported cut short, clock.cycle moves with its starts, a
//    running node's position jumps by whole missed cycles only, and what makes the other jumps (a restart, the default
//    resync.ms).
//  - S7: what a module-run converter relies on (docs/engine.md#module-run-converters): the module's node finishes its
//    cycle before the block it feeds wakes, in every cycle PipeWire's own profiler records, and its ports tell no
//    latency of their own, resampling or not, so its module's buffer enters the plan once, by moduleRunDelay.
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
#include <string_view>
#include <thread>
#include <vector>

#include <pthread.h>

#include <pipewire/filter.h>
#include <pipewire/impl.h>
#include <pipewire/pipewire.h>
// After pipewire.h, whose spa/utils/hook.h it needs.
#include <pipewire/extensions/profiler.h>
#include <spa/param/latency-utils.h>
#include <spa/param/profiler.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/result.h>

#include "../driver_tick.hpp"
#include "allocation_count.hpp"
#include "cycle_judge.hpp"
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
#include "sleeps.hpp"
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
			ear::sleeps::induce(std::chrono::seconds(2), "S2's data-loop stall");
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
	auto engine = Engine::create(EngineSettings{EAR_ENGINE_CONF, ear::SampleRate::fixed<kRate>(), block});
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
	explicit Upstream(Shared& shared, std::size_t capacity = kS2Capacity, Stalls* overruns = nullptr)
		: shared_(shared), overruns_(overruns), log(capacity), rawAtEntry(capacity), overran(capacity) {}
	void cycle(const spa_io_clock& clock, std::span<const float* const>, std::span<float* const> outputs,
	           std::uint32_t frames) noexcept override {
		const std::uint64_t sequence = shared_.sequence.fetch_add(1, std::memory_order_acq_rel);
		const std::uint64_t raw = rawFramesNow();
		std::fill_n(outputs[0], frames, rampAt(clock.position));
		if (!shared_.recording.load(std::memory_order_acquire)) return;
		if (const auto n = log.add(clock.position, sequence)) {
			rawAtEntry[*n] = raw;
			overran[*n] = overruns_ != nullptr && overruns_->plan().planned(*n) ? 1 : 0;
			log.commit(*n);
			if (overruns_ != nullptr) (*overruns_)(*n, clock.duration, ear::SampleRate::fixed<kRate>());
		}
	}
	std::size_t overrunCount() const { return overruns_ != nullptr ? overruns_->made() : 0; }

private:
	Shared& shared_;
	Stalls* overruns_;

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
				  ear::sleeps::pace(std::chrono::microseconds(50), "the invoke storm's rate between bursts of posts");
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
/// kS5Capacity frames into its output and keeps time as a source whose clock skips the graph's jump would: a cycle
/// whose position jumped ahead of the last one's end by at most the capacity skips that many frames and counts them
/// played; any other
/// jump is a new clock, played on from where the ring is (so `total - (position - first position)` takes a new value
/// there, the anchor S5's model re-takes). `total` counts frames played and skipped, as a source's totalFrames. The
/// ring's frame k is rampAt(k), and the writer side is not under test: the ring is kept full, each frame consumed
/// replaced by the one kS5Capacity later. B is no block, so it states each cycle's facts itself, by the rule a block
/// runs (CycleAccount over the engine's record of the internal clock's cuts, docs/engine.md#graphcycle), from the same
/// tick a block builds (driver_tick.hpp). It records, per cycle, the stamp of what it read from U, whether it took a new clock and the
/// facts; it publishes the position and first ring frame of the cycle it plays, for D in the same cycle; and it
/// overruns `overruns` of its recorded cycles once it has played.
class RingSource final : public ProbeCode {
public:
	RingSource(Shared& shared, Waiter& waiter, StallPlan overruns, const Engine& engine)
		: shared_(shared), waiter_(waiter), overruns_(std::move(overruns)), internalClock_(engine.internalClockId()),
		  cuts_(engine.cuts()), account_(FrameClock(ear::SampleRate::fixed<kRate>()), &engine.cuts()),
		  ring_(kS5Capacity), log(kS5Log), totalBefore(kS5Log), stamp(kS5Log), uniform(kS5Log), overran(kS5Log),
		  newClock(kS5Log), facts(kS5Log) {
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
		// The cycle as its driver states it, by the helper the port block builds its tick with.
		const CycleFacts stated = account_.next(driverTick(clock, internalClock_, cuts_).tick, kS5Capacity).facts;
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
				facts[*n] = stated;
				log.commit(*n);
				overruns_(*n, clock.duration, ear::SampleRate::fixed<kRate>());
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
	const std::uint32_t internalClock_;
	const CutRecord& cuts_;
	// Data thread only.
	CycleAccount account_;
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
	std::vector<CycleFacts> facts;          // per cycle: what B states of it
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

// ---------------------------------------------------------------------------------------------------------------------
// S6

/// S6's account of the internal clock, from its own realtime events (impl-node.c, node_ready): each cycle it starts
/// (`start`, after clock.cycle++ and before any follower is triggered), each it reports cut short (`incomplete`, in the
/// next node_ready before that cycle's `start`) and each it reports complete (`complete`: from the driver's own process
/// once every follower finished, or from the recovery in the next node_ready, which runs with
/// SPA_IO_CLOCK_FLAG_XRUN_RECOVER set on the driver's clock). Cycles are numbered from 1 by its own count, as
/// the engine's DriverCuts does (docs/engine.md#graphcycle); it reads the driver's clock (clock.cycle, the flag) through the pointer a probe's
/// process was handed, which is the driver's shared position that every follower reads.
class S6Driver {
public:
	S6Driver(Engine& engine, std::size_t capacity)
		: cut_(capacity), completes_(capacity), recovered_(capacity), clockCycle_(capacity), clockKnown_(capacity) {
		struct Search {
			pw_impl_node* found = nullptr;
		} search;
		pw_context_for_each_global(
			engine.context(),
			[](void* data, pw_global* global) {
				if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Node)) return 0;
				auto* node = static_cast<pw_impl_node*>(pw_global_get_object(global));
				const char* name = pw_properties_get(pw_impl_node_get_properties(node), PW_KEY_NODE_NAME);
				if (name == nullptr || std::string_view(name) != "Dummy-Driver") return 0;
				static_cast<Search*>(data)->found = node;
				return 1;
			},
			&search);
		if (search.found == nullptr) FAIL("the engine holds no node named Dummy-Driver");
		node_ = search.found;
		events_.version = PW_VERSION_IMPL_NODE_RT_EVENTS;
		events_.start = &S6Driver::onStart;
		events_.complete = &S6Driver::onComplete;
		events_.incomplete = &S6Driver::onIncomplete;
		pw_impl_node_add_rt_listener(node_, &hook_, &events_, this);
	}
	S6Driver(const S6Driver&) = delete;
	S6Driver& operator=(const S6Driver&) = delete;
	~S6Driver() { pw_impl_node_remove_rt_listener(node_, &hook_); }

	/// Data thread, from a probe's process: the driver's clock as followers see it (a probe's mapping of it).
	void see(const spa_io_clock& clock) noexcept {
		if (forgotten_.load(std::memory_order_relaxed)) return;
		const spa_io_clock* none = nullptr;
		clock_.compare_exchange_strong(none, &clock, std::memory_order_relaxed);
	}
	/// Wakes `waiter` at every cycle's start (an eventfd write on the data thread), for a test with no probe running.
	void wakeOnStart(Waiter* waiter) noexcept { waiter_.store(waiter, std::memory_order_relaxed); }
	/// Main thread, before the probes go: the listener stops reading their mapping of the clock, which goes with them.
	void forget() noexcept {
		forgotten_.store(true, std::memory_order_relaxed);
		clock_.store(nullptr, std::memory_order_relaxed);
	}
	/// Cycles started since this listened; inside a process, the one running. Any thread.
	std::uint64_t started() const noexcept { return started_.load(std::memory_order_acquire); }
	bool held(std::uint64_t g) const noexcept { return g > 0 && g <= cut_.size(); }
	bool cut(std::uint64_t g) const noexcept { return held(g) && cut_[g - 1].load(std::memory_order_acquire) != 0; }
	int completes(std::uint64_t g) const noexcept {
		return held(g) ? completes_[g - 1].load(std::memory_order_acquire) : -1;
	}
	/// Its complete came from the recovery (XRUN_RECOVER set): cut short, or the driver's own wake not yet run.
	bool recovered(std::uint64_t g) const noexcept {
		return held(g) && recovered_[g - 1].load(std::memory_order_acquire) != 0;
	}
	std::optional<std::uint32_t> clockCycle(std::uint64_t g) const noexcept {
		if (!held(g) || clockKnown_[g - 1].load(std::memory_order_acquire) == 0) return std::nullopt;
		return clockCycle_[g - 1].load(std::memory_order_acquire);
	}
	std::uint64_t cuts() const noexcept { return cuts_.load(std::memory_order_acquire); }

private:
	static void onStart(void* data) noexcept {
		auto* self = static_cast<S6Driver*>(data);
		const std::uint64_t g = self->started_.load(std::memory_order_relaxed) + 1;
		if (self->held(g)) {
			if (const spa_io_clock* clock = self->clock_.load(std::memory_order_relaxed)) {
				self->clockCycle_[g - 1].store(clock->cycle, std::memory_order_relaxed);
				self->clockKnown_[g - 1].store(1, std::memory_order_relaxed);
			}
		}
		self->started_.store(g, std::memory_order_release);
		if (Waiter* waiter = self->waiter_.load(std::memory_order_relaxed)) waiter->wake();
	}
	static void onComplete(void* data) noexcept {
		auto* self = static_cast<S6Driver*>(data);
		const std::uint64_t g = self->started_.load(std::memory_order_relaxed);
		if (!self->held(g)) return;
		self->completes_[g - 1].fetch_add(1, std::memory_order_release);
		const spa_io_clock* clock = self->clock_.load(std::memory_order_relaxed);
		if (clock != nullptr && (clock->flags & SPA_IO_CLOCK_FLAG_XRUN_RECOVER) != 0)
			self->recovered_[g - 1].store(1, std::memory_order_release);
	}
	static void onIncomplete(void* data) noexcept {
		auto* self = static_cast<S6Driver*>(data);
		const std::uint64_t g = self->started_.load(std::memory_order_relaxed);
		if (self->held(g)) self->cut_[g - 1].store(1, std::memory_order_release);
		self->cuts_.fetch_add(1, std::memory_order_release);
	}

	pw_impl_node* node_ = nullptr;
	pw_impl_node_rt_events events_{};
	spa_hook hook_{};
	std::atomic<const spa_io_clock*> clock_{nullptr};
	std::atomic<bool> forgotten_{false};
	std::atomic<Waiter*> waiter_{nullptr};
	std::vector<std::atomic<std::uint8_t>> cut_;
	std::vector<std::atomic<int>> completes_;
	std::vector<std::atomic<std::uint8_t>> recovered_;
	std::vector<std::atomic<std::uint32_t>> clockCycle_;
	std::vector<std::atomic<std::uint8_t>> clockKnown_;
	std::atomic<std::uint64_t> started_{0};
	std::atomic<std::uint64_t> cuts_{0};
};

/// One cycle a probe ran, as its process saw the driver's clock.
struct S6Run {
	std::uint64_t graph;    // S6Driver::started() inside the process: the driver's cycle by the listener's count
	std::uint64_t position; // clock.position
	std::uint64_t duration; // clock.duration
	std::uint64_t xrun;     // clock.xrun (set only by an ALSA driver)
	std::int64_t rawNs;     // CLOCK_MONOTONIC_RAW at entry
	std::uint32_t clockCycle;
	std::uint32_t flags; // clock.flags
};

/// S6's probe: records every cycle it runs while `recording`, and stalls the data thread at its planned runs.
class S6Probe final : public ProbeCode {
public:
	S6Probe(Waiter& waiter, S6Driver& driver, const std::atomic<bool>& recording, std::size_t capacity, StallPlan plan)
		: waiter_(waiter), driver_(driver), recording_(recording), runs_(capacity), stalls_(std::move(plan)) {}
	void cycle(const spa_io_clock& clock, std::span<const float* const>, std::span<float* const> outputs,
	           std::uint32_t frames) noexcept override {
		driver_.see(clock);
		for (float* out : outputs) std::fill_n(out, frames, 0.0F);
		if (recording_.load(std::memory_order_acquire)) {
			const std::size_t n = count_.load(std::memory_order_relaxed);
			if (n < runs_.size()) {
				timespec now{};
				clock_gettime(CLOCK_MONOTONIC_RAW, &now);
				runs_[n] = S6Run{driver_.started(),
				                 clock.position,
				                 clock.duration,
				                 clock.xrun,
				                 static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec,
				                 clock.cycle,
				                 clock.flags};
				count_.store(n + 1, std::memory_order_release);
				stalls_(n, clock.duration, ear::SampleRate::fixed<kRate>());
			} else {
				full_.store(true, std::memory_order_relaxed);
			}
		}
		waiter_.wake();
	}
	std::size_t count() const noexcept { return count_.load(std::memory_order_acquire); }
	const S6Run& run(std::size_t n) const noexcept { return runs_[n]; }
	bool full() const noexcept { return full_.load(std::memory_order_relaxed); }
	std::size_t stalls() const noexcept { return stalls_.made(); }
	std::size_t planned() const noexcept { return stalls_.plan().size(); }

private:
	Waiter& waiter_;
	S6Driver& driver_;
	const std::atomic<bool>& recording_;
	std::vector<S6Run> runs_;
	std::atomic<std::size_t> count_{0};
	std::atomic<bool> full_{false};
	Stalls stalls_;
};

/// What S6 finds in one probe's runs against the driver's account.
struct S6Findings {
	std::size_t runs = 0;
	std::size_t gaps = 0;      // consecutive runs more than one graph cycle apart
	std::size_t missed = 0;    // graph cycles between them
	std::size_t missedCut = 0; // of them, reported cut short
	std::size_t silent = 0;    // of them, not reported cut short: a silent loss
	std::size_t twice = 0;     // a run in the same or an earlier graph cycle than the last
	std::size_t notWhole = 0;  // a position jump other than the missed cycles' frames
	std::size_t recoverSeen = 0;
	std::size_t xrunSeen = 0;
	std::vector<std::uint32_t> offsets; // distinct clock.cycle - graph (mod 2^32)
	std::string firstSilent, firstNotWhole;
};

S6Findings judgeS6(const S6Probe& probe, const S6Driver& driver) {
	S6Findings found;
	found.runs = probe.count();
	for (std::size_t n = 0; n < found.runs; ++n) {
		const S6Run& run = probe.run(n);
		const auto offset = static_cast<std::uint32_t>(run.clockCycle - static_cast<std::uint32_t>(run.graph));
		if (std::ranges::find(found.offsets, offset) == found.offsets.end()) found.offsets.push_back(offset);
		if ((run.flags & SPA_IO_CLOCK_FLAG_XRUN_RECOVER) != 0) ++found.recoverSeen;
		if (run.xrun != 0) ++found.xrunSeen;
		if (n == 0) continue;
		const S6Run& last = probe.run(n - 1);
		if (run.graph <= last.graph) {
			++found.twice;
			continue;
		}
		const std::uint64_t missed = run.graph - last.graph - 1;
		if (missed > 0) ++found.gaps;
		found.missed += missed;
		for (std::uint64_t g = last.graph + 1; g < run.graph; ++g) {
			if (driver.cut(g)) {
				++found.missedCut;
				continue;
			}
			if (found.silent++ == 0)
				found.firstSilent = "run " + std::to_string(n) + ": graph cycle " + std::to_string(g) + " of " +
				                    std::to_string(last.graph) + ".." + std::to_string(run.graph) +
				                    " not cut, completes " + std::to_string(driver.completes(g)) + ", recovered " +
				                    std::to_string(driver.recovered(g) ? 1 : 0);
		}
		const auto jump =
			static_cast<std::int64_t>(run.position) - static_cast<std::int64_t>(last.position + last.duration);
		if (jump != static_cast<std::int64_t>(missed * last.duration) && found.notWhole++ == 0)
			found.firstNotWhole = "run " + std::to_string(n) + ": jump " + std::to_string(jump) + " frames across " +
			                      std::to_string(missed) + " missed cycles of " + std::to_string(last.duration) +
			                      ", raw " + std::to_string(run.rawNs - last.rawNs) + " ns since the last";
	}
	return found;
}

/// Stalls at runs `first`, `first + every`, ..., cycling through `lengths` (in cycles), `count` of them.
StallPlan s6Plan(std::size_t first, std::size_t every, std::size_t count, const std::vector<double>& lengths) {
	std::vector<StallPlan::At> at;
	for (std::size_t k = 0; k < count; ++k)
		at.push_back({first + k * every, StallLength::cycles(lengths[k % lengths.size()])});
	return StallPlan::at(std::move(at));
}

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
		ear::sleeps::pace(std::chrono::microseconds(200),
		                  "posts 200 us apart, so 3,000 of them span 0.6 s inside the stall");
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
	// of them, and nothing else, must be a new clock to B. Every cycle B ran goes through the one judge
	// (docs/testing.md#the-cycle-judge), whose stalls are U's overruns: each jump must be cycles the driver cut, by
	// their frames and within the raw clock, and every overrun of U's excused. Part B, recorded only: in B's
	// first cycle after one U ran and B did not, whether B read U's frames of its own cycle ("current") or of the cut one
	// ("stale"); counted apart for cycles cut by an overrun and the rest.
	StormCounts stormCounts; // before the engine, as S2's
	auto engine = engineAt(kS2Block);
	Waiter waiter(*engine);
	Wiring wiring(*engine, waiter);
	Shared shared;
	ear::test::CycleJudge judge(
		ear::test::JudgePlan{.judged = 1000,
	                         .induce = overruns(kS5OverrunEvery, kS5OverrunEvery / 4, kS5Overruns),
	                         .expected = {.starts = 0}},
		FrameClock(ear::SampleRate::fixed<kRate>()), &engine->cuts());
	Upstream upCode(shared, kS5Log, &judge.stalls());
	RingSource sourceCode(shared, waiter, overruns(kS5OverrunEvery, kS5OverrunEvery * 3 / 4, kS5Overruns), *engine);
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
	std::size_t jumps = 0, beyond = 0, clocks = 0, clockNotBeyond = 0;
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
		// What lost the cycles between is the judge's (below); the model needs only how far the position moved.
		if (lastEnd && at > *lastEnd) {
			++jumps;
			largestJump = std::max(largestJump, at - *lastEnd);
			if (cutSinceB)
				largestJumpAfterCut[cutKind] = std::max<std::size_t>(largestJumpAfterCut[cutKind], at - *lastEnd);
			if (at - *lastEnd > kS5Capacity) ++beyond;
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
		" frames: %zu after an overrun, %zu after another cut; %zu beyond its capacity of %u; %" PRIu64
		" new clocks, the model's anchor re-taken at each), %zu drifts of total - (position "
		"- first) (%zu after an overrun, %zu after another cut); D heard %zu cycles B ran too, %zu not the frame B "
		"played at that position. B: after %zu "
		"cycles U overran B read current %zu, stale %zu, none %zu, other %zu; after %zu other cut cycles current %zu, "
		"stale %zu, none %zu, other %zu; cut at its own position %zu; in its next cycle current %zu, other %zu; after "
		"cycles it ran, current %zu, the previous cycle's %zu, other %zu; %zu reads not one stamp\n",
		busyThreads, seconds, timeline.cycles, timeline.whole, timeline.onlyU, uOverranCut, uOverran, otherAfterU,
		timeline.onlyUB, bOverranCut, bOverran, otherAfterB, stormCounts.posts.load(), bCycles, jumps, largestJump,
		largestJumpAfterCut[1], largestJumpAfterCut[0], beyond, kS5Capacity, sourceCode.newClocks(), drifts,
		driftsAfterCut[1], driftsAfterCut[0], both, dWrong, afterCut[1], current[1], stale[1], none[1], other[1],
		afterCut[0], current[0], stale[0], none[0], other[0], same, nextCurrent, nextOther, steadyCurrent,
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
	for (std::size_t n = 0; n < sourceCode.log.count.load(); ++n) judge.at(sourceCode.facts[n], kS2Block);
	// Every jump was cycles the driver cut, by their frames and within the time the graph lost (so one past the
	// capacity only after a stall that long), by the one judge; each jump beyond the capacity was a new clock to B, and
	// B took no other.
	INFO(judge.report());
	CHECK(judge.report().ok());
	CHECK(clocks == beyond);
	CHECK(clockNotBeyond == 0);
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
		RtpReceiverShape{*RtpAddress::parse("0.0.0.0"), ear::test::freeUdpPort(), RtpEncoding::l24,
	                     ear::SampleRate::fixed<kRate>(), 100},
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

// S6 (cc60464df4; its findings in docs/engine.md#the-one-data-loop-and-invokes): what PipeWire 1.4.2 reports of every
// graph cycle, as DriverCuts and CycleAccount read it.
namespace {
constexpr std::size_t kS6Cycles = 1 << 17; // graph cycles the listener keeps: 175 s at S2's block
constexpr std::size_t kS6Runs = 1 << 16;   // runs each probe keeps
constexpr std::uint64_t kS6Graph = 6000;   // graph cycles recorded at least (8 s at S2's block)
} // namespace

TEST_CASE("S6: every graph cycle a node misses is one the driver reported cut short, across stalls of 1.5 to 150 "
          "cycles in each node of a chain and a sibling, beside a busy thread per core",
          "[spike][s2][s6]") {
	// u → m → d, and s beside them (the driver's follower only), at S2's block beside a busy thread per core. Each
	// probe stalls the data thread at its own planned runs, 1.5 to 150 cycles: a stall in u or m cuts the cycle after it
	// (docs/engine.md#the-one-data-loop-and-invokes) and longer ones make the driver's catch-up burst (its timer, armed
	// on the last timeout plus a cycle, fires back to back); a stall in d, the last node, finishes the graph after the
	// next timeout is ready, so node_ready finds the driver TRIGGERED and runs its recovery without reporting a cut. The
	// host adds its own cuts. Per probe, between any two cycles it ran, every graph cycle it did not run must be one the
	// driver reported cut short (`incomplete`): the design's `missedCut == missed`. Also gathered: clock.cycle against
	// the listener's count, the position's jumps against the missed cycles' frames, and whether a follower ever reads
	// a mark of the recovery (XRUN_RECOVER in clock.flags, clock.xrun).
	auto engine = engineAt(kS2Block);
	Waiter waiter(*engine);
	Wiring wiring(*engine, waiter);
	S6Driver driver(*engine, kS6Cycles);
	std::atomic<bool> recording{false};
	S6Probe upCode(waiter, driver, recording, kS6Runs,
	               s6Plan(20, 61, 36, {1.5, 1.5, 2.5, 4.0, 10.0, 40.0, 1.5, 150.0, 1.5}));
	S6Probe midCode(waiter, driver, recording, kS6Runs, s6Plan(40, 97, 16, {1.5, 3.0}));
	S6Probe downCode(waiter, driver, recording, kS6Runs, s6Plan(30, 89, 16, {1.5, 2.5}));
	S6Probe sideCode(waiter, driver, recording, kS6Runs, s6Plan(50, 131, 10, {1.5, 6.0}));
	Probe up(*engine, waiter, "u", 0, 1, upCode);
	Probe mid(*engine, waiter, "m", 1, 1, midCode);
	Probe down(*engine, waiter, "d", 1, 0, downCode);
	Probe side(*engine, waiter, "s", 0, 1, sideCode);
	const struct Forget {
		S6Driver& driver;
		~Forget() { driver.forget(); } // before the probes go
	} forget{driver};
	REQUIRE(waiter.until([&] { return up.streaming() && mid.streaming() && down.streaming() && side.streaming(); }));
	REQUIRE(wiring.link(waiter, {"u", "out.1"}, {"m", "in.1"}));
	REQUIRE(wiring.link(waiter, {"m", "out.1"}, {"d", "in.1"}));
	const std::uint64_t settled = driver.started();
	REQUIRE(waiter.until([&] { return driver.started() > settled + 16; }));

	const std::array<const S6Probe*, 4> codes{&upCode, &midCode, &downCode, &sideCode};
	const std::array<const char*, 4> names{"u", "m", "d", "s"};
	std::optional<BusyCores> busy;
	busy.emplace();
	const std::uint64_t from = driver.started();
	recording.store(true, std::memory_order_release);
	const auto start = std::chrono::steady_clock::now();
	const bool done = waiter.until(
		[&] {
			bool made = true, full = false;
			for (const S6Probe* code : codes) {
				made = made && code->stalls() >= code->planned();
				full = full || code->full();
			}
			return (made && driver.started() >= from + kS6Graph) || full || driver.started() + 1000 >= kS6Cycles;
		},
		90s);
	recording.store(false, std::memory_order_release);
	const double seconds = millisecondsSince(start) / 1000.0;
	const std::size_t busyThreads = busy->count();
	busy.reset();
	const std::uint64_t until = driver.started();
	REQUIRE(waiter.until([&] { return driver.started() > until + 2; })); // every recorded cycle's cut is told
	REQUIRE(done);
	for (const S6Probe* code : codes) {
		REQUIRE_FALSE(code->full());
		REQUIRE(code->stalls() == code->planned());
	}
	REQUIRE(driver.started() + 2 < kS6Cycles);

	// The driver's own account of every cycle recorded.
	std::size_t cuts = 0, triggered = 0, notOnce = 0, cycleUnknown = 0;
	std::vector<std::uint32_t> offsets;
	for (std::uint64_t g = from + 1; g <= until; ++g) {
		if (driver.cut(g)) ++cuts;
		else if (driver.recovered(g)) ++triggered; // the TRIGGERED path: complete from the recovery, no cut
		if (driver.completes(g) != 1) ++notOnce;
		if (const auto cycle = driver.clockCycle(g)) {
			const auto offset = static_cast<std::uint32_t>(*cycle - static_cast<std::uint32_t>(g));
			if (std::ranges::find(offsets, offset) == offsets.end()) offsets.push_back(offset);
		} else {
			++cycleUnknown;
		}
	}
	std::printf("S6: %" PRIu64 " graph cycles in %.1f s beside %zu busy threads: %zu reported cut short, %zu completed "
	            "by the TRIGGERED recovery, %zu not completed exactly once; clock.cycle - the listener's count: %zu "
	            "distinct (%u)\n",
	            until - from, seconds, busyThreads, cuts, triggered, notOnce, offsets.size(),
	            offsets.empty() ? 0U : offsets[0]);
	CHECK(notOnce == 0);
	CHECK(cycleUnknown == 0);
	CHECK(offsets.size() == 1);
	CHECK(cuts >= upCode.planned() + midCode.planned() + sideCode.planned());

	for (std::size_t p = 0; p < codes.size(); ++p) {
		const S6Findings found = judgeS6(*codes[p], driver);
		std::printf(
			"S6 %s: %zu runs, %zu gaps over %zu missed graph cycles, %zu of them cut, %zu silent; %zu runs not "
			"after the last; %zu jumps not the missed cycles' frames; XRUN_RECOVER read in %zu runs, clock.xrun "
			"set in %zu; clock.cycle - graph: %zu distinct (%u)\n",
			names[p], found.runs, found.gaps, found.missed, found.missedCut, found.silent, found.twice, found.notWhole,
			found.recoverSeen, found.xrunSeen, found.offsets.size(), found.offsets.empty() ? 0U : found.offsets[0]);
		INFO("probe " << names[p] << ": first silent loss: " << found.firstSilent);
		INFO("first jump not whole cycles: " << found.firstNotWhole);
		CHECK(found.silent == 0);
		CHECK(found.twice == 0);
		CHECK(found.notWhole == 0);
		CHECK(found.recoverSeen == 0); // a follower never runs inside the recovery: no mark to read
		CHECK(found.xrunSeen == 0);
		CHECK(found.offsets == offsets); // clock.cycle and the listener's count move together, a fixed offset apart
	}
}

TEST_CASE("S6: a node taken out of the graph misses cycles the driver never cut, and the internal clock started again "
          "re-anchors its position on the raw clock, a jump of any number of frames with no cycle missed",
          "[spike][s6]") {
	// Two ways a node stops hearing the clock without a stall. (a) Its pw_filter made inactive: the dummy driver keeps
	// running (the inactive node's activation is INACTIVE, so node_ready skips it), and the node, active again, has
	// missed every cycle started meanwhile, none of them cut, its position moved by exactly their frames: the design's
	// `absent`. (b) Every node it drives gone: it stops (state IDLE), and started again node-driver.c's do_start clears
	// last_time, so its first timeout puts the position on the raw clock (on_timeout: position = current_position)
	// instead of moving it on by a cycle. The next node's first run is then the driver's next cycle by count, with the
	// position moved by the raw time the clock stood still: the design's `reposition`, any number of frames.
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Registry registry(*engine, waiter);
	S6Driver driver(*engine, 1 << 16);
	driver.forget();             // probes come and go here: the listener never reads a probe's mapping of the clock
	driver.wakeOnStart(&waiter); // (a) waits for cycles no probe runs
	const std::atomic<bool> recording{true};
	const auto rawNow = [] {
		timespec now{};
		clock_gettime(CLOCK_MONOTONIC_RAW, &now);
		return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
	};
	const auto running = [&] { return registry.state("Dummy-Driver") == PW_NODE_STATE_RUNNING; };

	// (a) inactive, three times.
	{
		S6Probe code(waiter, driver, recording, 1 << 14, StallPlan{});
		Probe probe(*engine, waiter, "p", 0, 1, code);
		REQUIRE(waiter.until([&] { return probe.streaming() && code.count() >= 16 && running(); }));
		for (int round = 0; round < 3; ++round) {
			const std::size_t before = code.count();
			probe.setActive(false);
			const std::uint64_t startedThen = driver.started();
			REQUIRE(waiter.until([&] { return driver.started() >= startedThen + 64; })); // the driver runs on
			const std::int64_t reactivatedNs = rawNow();
			probe.setActive(true);
			REQUIRE(waiter.until([&] { return code.count() >= before + 16; }));
			std::size_t k = before;
			while (k < code.count() && code.run(k).rawNs < reactivatedNs) ++k;
			REQUIRE(k > 0);
			REQUIRE(k < code.count());
			const S6Run& last = code.run(k - 1);
			const S6Run& next = code.run(k);
			const std::uint64_t missed = next.graph - last.graph - 1;
			std::uint64_t cut = 0;
			for (std::uint64_t g = last.graph + 1; g < next.graph; ++g) cut += driver.cut(g) ? 1 : 0;
			const auto jump =
				static_cast<std::int64_t>(next.position) - static_cast<std::int64_t>(last.position + last.duration);
			std::printf("S6 inactive %d: %" PRIu64 " cycles missed, %" PRIu64 " of them cut; jump %" PRId64
			            " frames (%" PRId64 " blocks); clock.cycle +%u\n",
			            round, missed, cut, jump, jump / static_cast<std::int64_t>(kBlock),
			            next.clockCycle - last.clockCycle);
			INFO("inactive round " << round);
			CHECK(missed >= 64);
			CHECK(cut == 0);
			CHECK(jump == static_cast<std::int64_t>(missed * last.duration));
			CHECK(next.clockCycle - last.clockCycle == missed + 1);
		}
	}

	// (b) gone, then a new node, three times; each new node's first run against the last node's last.
	std::optional<S6Run> last;
	for (int round = 0; round <= 3; ++round) {
		auto code = std::make_unique<S6Probe>(waiter, driver, recording, 1 << 14, StallPlan{});
		auto probe = std::make_unique<Probe>(*engine, waiter, "p", 0, 1, *code);
		REQUIRE(waiter.until([&] { return probe->streaming() && code->count() >= 16 && running(); }));
		if (last) {
			const S6Run& next = code->run(0);
			const std::uint64_t missed = next.graph - last->graph - 1;
			const auto jump =
				static_cast<std::int64_t>(next.position) - static_cast<std::int64_t>(last->position + last->duration);
			const std::int64_t rawFrames = (next.rawNs - last->rawNs) * kRate / 1'000'000'000;
			std::printf("S6 restart %d: %" PRIu64 " cycles started between the two nodes; jump %" PRId64
			            " frames (%" PRId64 " blocks and %" PRId64 " frames), raw time between the runs %" PRId64
			            " frames; clock.cycle +%u\n",
			            round, missed, jump, jump / static_cast<std::int64_t>(kBlock),
			            jump % static_cast<std::int64_t>(kBlock), rawFrames, next.clockCycle - last->clockCycle);
			INFO("restart round " << round);
			CHECK(missed == 0);
			CHECK(next.clockCycle == last->clockCycle + 1);
			CHECK(std::abs(jump + static_cast<std::int64_t>(last->duration) - rawFrames) <=
			      static_cast<std::int64_t>(kBlock));
		}
		probe.reset();
		const bool stopped = waiter.until([&] { return registry.state("Dummy-Driver") != PW_NODE_STATE_RUNNING; });
		INFO("round " << round);
		REQUIRE(stopped);
		last = code->run(code->count() - 1); // its very last run, now none can follow
	}
}

TEST_CASE("S6: with node-driver.c's default resync.ms a stall past 10 ms moves the position by a jump that is not "
          "whole cycles",
          "[spike][s6]") {
	// engine.conf without its resync.ms (docs/engine.md#the-internal-clock): what the sendspin clock test ran on when
	// it saw jumps of 1127, 1156 and 1189 frames (5bf75b4d1e, an hour before c1bb9d246b set resync.ms). Past 10 ms of
	// error the driver puts the position on the raw clock, so a 30 ms stall's jump is the raw time it lost, not whole
	// cycles, and the catch-up burst after it steps the position back at the next reset.
	ear::test::TempFolder folder("s6-resync");
	std::string conf;
	{
		std::ifstream in(EAR_ENGINE_CONF);
		for (std::string line; std::getline(in, line);)
			if (line.find("resync.ms") == std::string::npos) conf += line + "\n";
	}
	const std::filesystem::path path = folder.path() / "engine.conf";
	std::ofstream(path) << conf;
	for (const char* name : {"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE", "DBUS_SESSION_BUS_ADDRESS",
	                         "DBUS_SYSTEM_BUS_ADDRESS"})
		unsetenv(name);
	auto made = Engine::create(EngineSettings{path.string(), ear::SampleRate::fixed<kRate>(), kBlock});
	if (!made) FAIL("Engine::create: " << made.error().detail);
	auto engine = std::move(*made);
	Waiter waiter(*engine);
	S6Driver driver(*engine, 1 << 16);
	driver.forget(); // it needs only the listener's count, never the clock's mapping
	const std::atomic<bool> recording{true};
	S6Probe code(
		waiter, driver, recording, 1 << 14,
		StallPlan::at({{100, StallLength::of(30ms)}, {300, StallLength::of(30ms)}, {500, StallLength::of(30ms)}}));
	Probe probe(*engine, waiter, "p", 0, 1, code);
	REQUIRE(waiter.until([&] { return code.stalls() == 3 && code.count() >= 700; }, 30s));
	const S6Findings found = judgeS6(code, driver);
	std::size_t back = 0;
	for (std::size_t n = 1; n < found.runs; ++n) back += code.run(n).position <= code.run(n - 1).position ? 1 : 0;
	std::printf("S6 default resync.ms: %zu runs, %zu jumps not the missed cycles' frames (first: %s), %zu steps back, "
	            "%zu missed graph cycles, %zu silent\n",
	            found.runs, found.notWhole, found.firstNotWhole.c_str(), back, found.missed, found.silent);
	CHECK(found.notWhole > 0);
	CHECK(found.silent == 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// S7 (the module-run converter's facts, docs/engine.md#module-run-converters)

namespace {

/// PipeWire's activation status of a node that ran its cycle to the end (PW_NODE_ACTIVATION_FINISHED in 1.4.2's
/// private.h, which is not installed; module-profiler.c reports it as is).
constexpr std::int32_t kActivationFinished = 3;

/// One follower's cycle as module-profiler reports it: when it was signalled, woke and finished (CLOCK_MONOTONIC ns;
/// a node marked async reports its previous cycle's instead, module-profiler.c) and its status.
struct FollowerCycle {
	std::int64_t signal = 0;
	std::int64_t awake = 0;
	std::int64_t finish = 0;
	std::int32_t status = 0;
};

/// One driver cycle: module-profiler's count of it, the driver's signal, and the two followers the test watches.
struct ProfiledCycle {
	std::int64_t count = 0;
	std::int64_t driverSignal = 0;
	std::optional<FollowerCycle> upstream;
	std::optional<FollowerCycle> block;
};

/// PipeWire's own profiler (libpipewire-module-profiler, what pw-profiler and pw-top read), bound on ear's connection:
/// once a client binds it, it reports every cycle of every driver it knows. It learns drivers as they are added, so
/// the engine loads it before its Dummy-Driver is made (profiledEngine). Keeps the cycles `upstream` or `block` ran in.
class Profile {
public:
	Profile(Engine& engine, Waiter& waiter, std::string upstream, std::string block)
		: waiter_(waiter), upstream_(std::move(upstream)), block_(std::move(block)) {
		registry_ = pw_core_get_registry(engine.core(), PW_VERSION_REGISTRY, 0);
		pw_registry_add_listener(registry_, &registryHook_, &registryEvents(), this);
	}
	~Profile() {
		if (profiler_ != nullptr) {
			spa_hook_remove(&profilerHook_);
			pw_proxy_destroy(profiler_);
		}
		spa_hook_remove(&registryHook_);
		pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
	}
	Profile(const Profile&) = delete;
	Profile& operator=(const Profile&) = delete;

	const std::vector<ProfiledCycle>& cycles() const { return cycles_; }

private:
	static const pw_registry_events& registryEvents() {
		static const pw_registry_events table = [] {
			pw_registry_events events{};
			events.version = PW_VERSION_REGISTRY_EVENTS;
			events.global = &Profile::onGlobal;
			return events;
		}();
		return table;
	}
	static const pw_profiler_events& profilerEvents() {
		static const pw_profiler_events table = [] {
			pw_profiler_events events{};
			events.version = PW_VERSION_PROFILER_EVENTS;
			events.profile = &Profile::onProfile;
			return events;
		}();
		return table;
	}
	static void onGlobal(void* data, std::uint32_t id, std::uint32_t /*permissions*/, const char* type,
	                     std::uint32_t /*version*/, const spa_dict* /*props*/) {
		auto* self = static_cast<Profile*>(data);
		if (self->profiler_ != nullptr || std::string_view(type) != PW_TYPE_INTERFACE_Profiler) return;
		self->profiler_ = static_cast<pw_proxy*>(
			pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Profiler, PW_VERSION_PROFILER, 0));
		pw_profiler_add_listener(reinterpret_cast<pw_profiler*>(self->profiler_), &self->profilerHook_,
		                         &profilerEvents(), self);
		self->waiter_.wake();
	}
	/// A driverBlock or followerBlock: id, name, previous signal, signal, awake, finish, status, ...
	static FollowerCycle block(const spa_pod* value, std::string& name) {
		spa_pod_parser parser;
		spa_pod_parser_pod(&parser, value);
		std::int32_t id = 0;
		const char* named = nullptr;
		std::int64_t previous = 0;
		FollowerCycle cycle;
		spa_pod_parser_get_struct(&parser, SPA_POD_Int(&id), SPA_POD_String(&named), SPA_POD_Long(&previous),
		                          SPA_POD_Long(&cycle.signal), SPA_POD_Long(&cycle.awake), SPA_POD_Long(&cycle.finish),
		                          SPA_POD_Int(&cycle.status));
		name = named != nullptr ? named : "";
		return cycle;
	}
	static void onProfile(void* data, const spa_pod* pod) {
		auto* self = static_cast<Profile*>(data);
		if (!spa_pod_is_struct(pod)) return;
		// SPA_POD_STRUCT_FOREACH, spelt out: its iterator is a void* in C++.
		const void* body = SPA_POD_BODY_CONST(pod);
		for (auto* object = static_cast<const spa_pod*>(body); spa_pod_is_inside(body, SPA_POD_BODY_SIZE(pod), object);
		     object = static_cast<const spa_pod*>(spa_pod_next(object))) {
			if (!spa_pod_is_object_type(object, SPA_TYPE_OBJECT_Profiler)) continue;
			ProfiledCycle cycle;
			const spa_pod_prop* prop = nullptr;
			SPA_POD_OBJECT_FOREACH(reinterpret_cast<const spa_pod_object*>(object), prop) {
				std::string name;
				if (prop->key == SPA_PROFILER_info) {
					spa_pod_parser parser;
					spa_pod_parser_pod(&parser, &prop->value);
					spa_pod_parser_get_struct(&parser, SPA_POD_Long(&cycle.count));
				} else if (prop->key == SPA_PROFILER_driverBlock) {
					cycle.driverSignal = block(&prop->value, name).signal;
				} else if (prop->key == SPA_PROFILER_followerBlock) {
					const FollowerCycle seen = block(&prop->value, name);
					if (name == self->upstream_) cycle.upstream = seen;
					if (name == self->block_) cycle.block = seen;
				}
			}
			if (cycle.upstream || cycle.block) self->cycles_.push_back(cycle);
		}
		self->waiter_.wake();
	}

	Waiter& waiter_;
	std::string upstream_;
	std::string block_;
	pw_registry* registry_ = nullptr;
	spa_hook registryHook_{};
	pw_proxy* profiler_ = nullptr;
	spa_hook profilerHook_{};
	std::vector<ProfiledCycle> cycles_;
};

/// The engine on engine.conf with module-profiler loaded after its own modules, so it sees the Dummy-Driver made.
std::unique_ptr<Engine> profiledEngine(const ear::test::TempFolder& folder) {
	std::string conf;
	{
		std::ifstream in(EAR_ENGINE_CONF);
		for (std::string line; std::getline(in, line);) {
			conf += line + "\n";
			if (line.find("{ name = libpipewire-module-adapter }") != std::string::npos)
				conf += "    { name = libpipewire-module-profiler }\n";
		}
	}
	REQUIRE(conf.find("libpipewire-module-profiler") != std::string::npos);
	const std::filesystem::path path = folder.path() / "engine.conf";
	std::ofstream(path) << conf;
	for (const char* name : {"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE", "DBUS_SESSION_BUS_ADDRESS",
	                         "DBUS_SYSTEM_BUS_ADDRESS"})
		unsetenv(name);
	auto made = Engine::create(EngineSettings{path.string(), ear::SampleRate::fixed<kRate>(), kBlock});
	if (!made) FAIL("Engine::create: " << made.error().detail);
	return std::move(*made);
}

/// The capture (upstream) latency one port tells, as any client reads it: a registry binding finds the port by its
/// node's global id and its name, binds it, and keeps its newest Latency param of direction output.
class PortLatency {
public:
	PortLatency(Engine& engine, Waiter& waiter, std::uint32_t nodeId, std::string portName)
		: waiter_(waiter), nodeId_(nodeId), portName_(std::move(portName)) {
		registry_ = pw_core_get_registry(engine.core(), PW_VERSION_REGISTRY, 0);
		pw_registry_add_listener(registry_, &registryHook_, &registryEvents(), this);
	}
	~PortLatency() {
		if (port_ != nullptr) {
			spa_hook_remove(&portHook_);
			pw_proxy_destroy(port_);
		}
		spa_hook_remove(&registryHook_);
		pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
	}
	PortLatency(const PortLatency&) = delete;
	PortLatency& operator=(const PortLatency&) = delete;

	std::optional<Latency> told() const { return told_; }
	std::size_t tellings() const { return tellings_; }

private:
	static const pw_registry_events& registryEvents() {
		static const pw_registry_events table = [] {
			pw_registry_events events{};
			events.version = PW_VERSION_REGISTRY_EVENTS;
			events.global = &PortLatency::onGlobal;
			return events;
		}();
		return table;
	}
	static const pw_port_events& portEvents() {
		static const pw_port_events table = [] {
			pw_port_events events{};
			events.version = PW_VERSION_PORT_EVENTS;
			events.param = &PortLatency::onParam;
			return events;
		}();
		return table;
	}
	static void onGlobal(void* data, std::uint32_t id, std::uint32_t /*permissions*/, const char* type,
	                     std::uint32_t /*version*/, const spa_dict* props) {
		auto* self = static_cast<PortLatency*>(data);
		if (self->port_ != nullptr || std::string_view(type) != PW_TYPE_INTERFACE_Port || props == nullptr) return;
		const char* node = spa_dict_lookup(props, PW_KEY_NODE_ID);
		const char* name = spa_dict_lookup(props, PW_KEY_PORT_NAME);
		if (node == nullptr || name == nullptr || std::to_string(self->nodeId_) != node || self->portName_ != name)
			return;
		self->port_ =
			static_cast<pw_proxy*>(pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Port, PW_VERSION_PORT, 0));
		pw_proxy_add_object_listener(self->port_, &self->portHook_, &portEvents(), self);
		std::uint32_t wanted = SPA_PARAM_Latency;
		pw_port_subscribe_params(reinterpret_cast<pw_port*>(self->port_), &wanted, 1);
	}
	static void onParam(void* data, int /*seq*/, std::uint32_t id, std::uint32_t /*index*/, std::uint32_t /*next*/,
	                    const spa_pod* param) {
		auto* self = static_cast<PortLatency*>(data);
		spa_latency_info info{};
		if (id != SPA_PARAM_Latency || param == nullptr || spa_latency_parse(param, &info) < 0 ||
		    info.direction != SPA_DIRECTION_OUTPUT)
			return;
		self->told_ = Latency{static_cast<std::uint32_t>(info.min_rate), static_cast<std::uint32_t>(info.max_rate)};
		++self->tellings_;
		self->waiter_.wake();
	}

	Waiter& waiter_;
	std::uint32_t nodeId_;
	std::string portName_;
	pw_registry* registry_ = nullptr;
	spa_hook registryHook_{};
	pw_proxy* port_ = nullptr;
	spa_hook portHook_{};
	std::optional<Latency> told_;
	std::size_t tellings_ = 0;
};

/// Writes zeros, and counts its runs and the frames of its inside input 1 that were not silent.
class Heard final : public AudioCode {
public:
	explicit Heard(Waiter& waiter) : waiter_(waiter) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const ear::GraphCycle&) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
		const auto sound = static_cast<std::uint64_t>(
			std::ranges::count_if(inputs[0].first(frames), [](float sample) { return sample != 0.0F; }));
		sound_.fetch_add(sound, std::memory_order_relaxed);
		runs_.fetch_add(1, std::memory_order_release);
		waiter_.wake();
	}
	void release() noexcept override {}
	std::uint64_t runs() const { return runs_.load(std::memory_order_acquire); }
	std::uint64_t sound() const { return sound_.load(std::memory_order_relaxed); }

private:
	Waiter& waiter_;
	std::atomic<std::uint64_t> runs_{0};
	std::atomic<std::uint64_t> sound_{0};
};

/// A block "fpp" (outputs stream.1, stream.2; the inside input bank from_rtp) running Heard, and the receivers.
struct S7Graph {
	explicit S7Graph(std::unique_ptr<Engine> made)
		: engine(std::move(made)), waiter(*engine), registry(*engine, waiter), blocks(*engine, bell),
		  receivers(*engine), heard(waiter), blockEvents(waiter), events(waiter) {
		block = blocks.make("fpp",
		                    Ports{{{Direction::output, "stream", {"1", "2"}},
		                           {Direction::input, "from_rtp", {"1", "2"}, PortBank::Reach::inside}}},
		                    kFormat, blockEvents);
		REQUIRE(ear::testing::driveNow(*block, &heard));
		REQUIRE(waiter.until([&] { return blockEvents.isReady; }));
	}
	~S7Graph() {
		receiver.reset();
		ear::testing::silenceOrAbort(*block);
	}
	void receive(const RtpReceiverShape& shape) {
		receiver = receivers.make(
			CompanionTarget{
				"fpp.rtp", "fpp", Direction::input, {"from_rtp.1", "from_rtp.2"}, kFormat, block->blockId()},
			shape, events);
		REQUIRE(waiter.until([&] { return events.isReady || events.lostDetail; }));
		REQUIRE(events.lostDetail.value_or("") == "");
	}
	/// The block ran `cycles` more times.
	bool runs(std::uint64_t cycles) {
		const std::uint64_t until = heard.runs() + cycles;
		return waiter.until([&] { return heard.runs() >= until; });
	}

	std::unique_ptr<Engine> engine;
	Waiter waiter;
	Registry registry;
	NoBell bell;
	FilterPortBlocks blocks;
	RtpReceivers receivers;
	Heard heard;
	BlockEvents blockEvents;
	CompanionEvents events;
	std::unique_ptr<PortBlock> block;
	std::unique_ptr<EngineCompanion> receiver;
};

} // namespace

TEST_CASE("S7: a module-run converter's node finishes before its block wakes, in every cycle the profiler records",
          "[spike][s7]") {
	// What set and tracked converters will rely on as module-run ones do: the stream feeding an inside input bank has
	// run its cycle before the block reads it, so the block reads this cycle's frames, never the last's. Measured by
	// PipeWire's own profiler on the real receiver, resampling a 44.1 kHz sender's tone, over 2,000 cycles.
	ear::test::TempFolder folder("s7-order");
	S7Graph graph(profiledEngine(folder));
	const RtpReceiverShape shape{*RtpAddress::parse("0.0.0.0"), ear::test::freeUdpPort(), RtpEncoding::l16,
	                             SampleRate::fixed<44100>(), 100};
	graph.receive(shape);
	ear::test::RtpSender sender(shape.port, ear::test::RtpPcm::l16, shape.rate.hz(),
	                            ear::test::RtpTone{{750.0, 1500.0}, 0.5});
	sender.send(60 * shape.rate.hz());
	REQUIRE(graph.waiter.until([&] { return graph.heard.sound() > 0; }));

	constexpr std::size_t kCycles = 2000;
	Profile profile(*graph.engine, graph.waiter, "fpp.rtp", "fpp");
	const auto both = [&] {
		return static_cast<std::size_t>(std::ranges::count_if(
			profile.cycles(), [](const ProfiledCycle& cycle) { return cycle.upstream && cycle.block; }));
	};
	REQUIRE(graph.waiter.until([&] { return both() >= kCycles; }, 60s));
	REQUIRE(graph.registry.props("fpp.rtp") != nullptr);
	const auto& props = *graph.registry.props("fpp.rtp");
	const auto async = props.find("node.async");

	std::size_t judged = 0, before = 0, earlier = 0, unfinished = 0, gaps = 0;
	std::int64_t least = INT64_MAX;
	std::string firstWrong;
	const std::vector<ProfiledCycle>& cycles = profile.cycles();
	for (std::size_t n = 0; n < cycles.size(); ++n) {
		const ProfiledCycle& cycle = cycles[n];
		if (n > 0 && cycle.count != cycles[n - 1].count + 1) ++gaps;
		if (!cycle.upstream || !cycle.block) continue;
		const FollowerCycle& upstream = *cycle.upstream;
		const FollowerCycle& block = *cycle.block;
		if (upstream.status != kActivationFinished || block.status != kActivationFinished) {
			++unfinished; // a cycle an xrun cut short: neither ran it to the end
			continue;
		}
		++judged;
		if (upstream.signal < cycle.driverSignal || block.signal < cycle.driverSignal) ++earlier;
		least = std::min(least, block.awake - upstream.finish);
		if (upstream.finish <= block.signal && block.signal <= block.awake) {
			++before;
		} else if (firstWrong.empty()) {
			firstWrong = "cycle " + std::to_string(cycle.count) + ": the receiver finished at " +
			             std::to_string(upstream.finish) + ", the block was signalled at " +
			             std::to_string(block.signal) + " and woke at " + std::to_string(block.awake);
		}
	}
	std::printf("S7 order: %zu cycles judged; in %zu the receiver finished before the block was signalled (least gap "
	            "to its wake %" PRId64 " ns); %zu reported from an earlier cycle; %zu cut short; %zu profiler gaps; "
	            "fpp.rtp node.async %s\n",
	            judged, before, least, earlier, unfinished, gaps,
	            async == props.end() ? "unset" : async->second.c_str());
	INFO(firstWrong);
	CHECK(judged >= kCycles / 2);
	CHECK(before == judged);
	CHECK(earlier == 0);
	CHECK((async == props.end() || async->second == "false"));
}

TEST_CASE("S7: a module-run converter's ports tell no latency of their own, resampling or not, so its module's "
          "buffer enters once, by moduleRunDelay",
          "[spike][s7]") {
	// docs/engine.md#latency-per-bank-pair: a module-run converter's delay is entered `beyond` its bank, which is right
	// only if nothing the engine carries already counts it. What rtp-source's output port tells, with its resampler at
	// 1:1 (a sender at the graph's rate) and converting (44.1 kHz into 48 kHz), once the sound has started, and what
	// the block then tells downstream.
	for (const std::uint32_t senderRate : {kRate, 44100U}) {
		INFO("the sender at " << senderRate << " Hz");
		S7Graph graph(makeEngine());
		const RtpReceiverShape shape{*RtpAddress::parse("0.0.0.0"), ear::test::freeUdpPort(), RtpEncoding::l16,
		                             *SampleRate::of(senderRate), 100};
		const Delay delay = moduleRunDelay(shape, FrameClock(ear::SampleRate::fixed<kRate>()));
		graph.block->carryLatency(LatencyPlan{.pairs = {{"from_rtp", "stream"}}, .beyond = {{"from_rtp", delay}}});
		PortLatency stream(*graph.engine, graph.waiter, graph.block->blockId(), "stream.1");
		REQUIRE(graph.waiter.until([&] { return stream.told() == delay.latency; }));

		graph.receive(shape);
		ear::test::RtpSender sender(shape.port, ear::test::RtpPcm::l16, senderRate,
		                            ear::test::RtpTone{{750.0, 1500.0}, 0.5});
		sender.send(10 * senderRate);
		PortLatency receiverPort(*graph.engine, graph.waiter, *graph.registry.id("fpp.rtp"), "receive_AUX0");
		REQUIRE(graph.waiter.until([&] { return receiverPort.told().has_value() && graph.heard.sound() > 0; }));
		REQUIRE(graph.runs(64)); // the main loop turned meanwhile, with any retell among it
		std::printf("S7 latency, sender at %u Hz: the receiver's port tells %u-%u frames (%zu times); stream tells "
		            "%u-%u; moduleRunDelay %u\n",
		            senderRate, receiverPort.told()->minFrames, receiverPort.told()->maxFrames, receiverPort.tellings(),
		            stream.told()->minFrames, stream.told()->maxFrames, delay.latency.maxFrames);
		CHECK(receiverPort.told() == Latency{0, 0});
		CHECK(stream.told() == delay.latency);
	}
}
