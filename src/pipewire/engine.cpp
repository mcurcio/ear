// PipeWire's engine inside EAR (docs/engine.md#properties-ear-sets; docs/engine.md#realtime-priority: CAP_SYS_NICE
// and the scheduling report; docs/engine.md#denormals: denormals flushed on the data thread).
#include "ear/pipewire/engine.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <ctime>
#include <set>
#include <string>
#include <utility>

#include <linux/capability.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include "callback_boundary.hpp"
#include "ear/pipewire/data_loop_wedge.hpp"
#include "engine_property_set.hpp"

namespace ear::pipewire {
namespace {

/// The two stop signals onStopSignal turns into main-loop events.
sigset_t stopSignals() {
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	return set;
}

/// The run image gives the binary CAP_SYS_NICE as a permitted-only file capability (a binary
/// marked effective fails to start wherever the capability is not allowed). Making it effective
/// here, before any engine thread exists, lets module-rt raise the data thread. Not permitted:
/// nothing changes and nothing fails; the scheduling report shows the result. Raw capget and
/// capset, so no libcap.
void raisePermittedSysNice() {
	__user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3, 0};
	__user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3]{};
	if (syscall(SYS_capget, &header, data) != 0) return;
	auto& word = data[CAP_TO_INDEX(CAP_SYS_NICE)];
	const auto bit = static_cast<std::uint32_t>(CAP_TO_MASK(CAP_SYS_NICE));
	if ((word.permitted & bit) == 0 || (word.effective & bit) != 0) return;
	word.effective |= bit;
	syscall(SYS_capset, &header, data);
}

/// How the data loop's thread is scheduled, read on that thread (read()) and said on the caller's (scheduling()), so
/// nothing is allocated on the data loop.
struct SchedulingRead {
	bool done = false;
	int policy = 0;
	int priority = 0;

	/// Runs on the data loop.
	void read() noexcept {
		policy = sched_getscheduler(0) & ~SCHED_RESET_ON_FORK;
		sched_param param{};
		sched_getparam(0, &param);
		priority = param.sched_priority;
		done = true;
	}

	Scheduling scheduling() const {
		if (!done) return {false, "unknown"};
		switch (policy) {
		case SCHED_FIFO:
			return {true, "SCHED_FIFO " + std::to_string(priority)};
		case SCHED_RR:
			return {true, "SCHED_RR " + std::to_string(priority)};
		case SCHED_OTHER:
			return {false, "SCHED_OTHER"};
		case SCHED_BATCH:
			return {false, "SCHED_BATCH"};
		case SCHED_IDLE:
			return {false, "SCHED_IDLE"};
		default:
			return {false, "policy " + std::to_string(policy)};
		}
	}
};

/// What the denormal flush is asked: the thread that invoked it, and whether the call ran elsewhere (on the data loop).
struct DenormalsFlush {
	pthread_t caller;
	std::atomic<bool> done{false};

	/// Runs on the data loop: the calling thread flushes denormal results to zero and reads denormal inputs as zero
	/// (FTZ and DAZ on x86, FZ on arm64), so a decaying signal never slows process() by orders of magnitude. PipeWire
	/// 1.4.2's cpu.zero.denormals sets them on the main thread instead in this engine (the data loop's setup runs
	/// inline there), and every thread made later inherits them, so it stays unset. A blocking invoke runs inline on
	/// the caller until the data loop's thread has entered its loop (loop.c treats a loop with no thread yet as its
	/// own), so the call does nothing when it runs on its caller, and says whether it ran on the data loop's thread.
	void flush() noexcept {
		if (pthread_equal(pthread_self(), caller) != 0) return; // inline: the loop has not entered yet
#if defined(__x86_64__) || defined(__i386__)
		_mm_setcsr(_mm_getcsr() | 0x8040U); // FTZ (bit 15) and DAZ (bit 6)
#elif defined(__aarch64__)
		std::uint64_t fpcr = 0;
		asm volatile("mrs %0, fpcr" : "=r"(fpcr));
		fpcr |= 1ULL << 24U; // FZ
		asm volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
		done.store(true, std::memory_order_release);
	}
};

/// Flushes denormals on the data loop's thread, waiting for that thread to enter its loop: pw_context_new starts it
/// and returns at once (data-loop.c, pw_data_loop_start), so the first invokes can run inline on this thread, which
/// DenormalsFlush::flush leaves alone. Nothing else waits on the thread, so it enters within microseconds; 5 s without
/// it is an engine that does not run.
bool flushDenormalsOnDataThread(pw_data_loop* dataLoop) {
	DenormalsFlush flush{.caller = pthread_self()};
	for (int attempt = 0; attempt < 5000 && !flush.done.load(std::memory_order_acquire); ++attempt) {
		if (invoke(DataLoop(dataLoop), realtime<&DenormalsFlush::flush>, &flush, true) < 0) return false;
		if (!flush.done.load(std::memory_order_acquire)) {
			const timespec pause{0, 1'000'000};
			nanosleep(&pause, nullptr);
		}
	}
	return flush.done.load(std::memory_order_acquire);
}

/// The clock settings EngineSettings decides, each key from the property table (docs/engine.md#properties-ear-sets says
/// why each). pw_context_new reads them from its own properties; engine.conf's context.properties would overwrite them,
/// so it sets none.
pw_properties* contextProperties(const EngineSettings& settings) {
	const auto rate = std::to_string(settings.graphFrequency);
	const auto block = std::to_string(settings.blockSize);
	return PropertySet<Home::context>({{property<Home::context>("config.name"), settings.configFile.string()},
	                                   {property<Home::context>("default.clock.rate"), rate},
	                                   {property<Home::context>("default.clock.allowed-rates"), "[ " + rate + " ]"},
	                                   {property<Home::context>("default.clock.quantum"), block},
	                                   {property<Home::context>("default.clock.min-quantum"), block},
	                                   {property<Home::context>("default.clock.max-quantum"), block},
	                                   {property<Home::context>("default.clock.quantum-floor"), block},
	                                   {property<Home::context>("default.clock.quantum-limit"), block},
	                                   {property<Home::context>("clock.power-of-two-quantum"), "false"}})
	    .release();
}

} // namespace

struct Engine::Impl {
	bool boundaryInstalled = false;
	sigset_t previousMask{};
	bool initialised = false;
	pw_main_loop* loop = nullptr;
	pw_context* context = nullptr;
	pw_core* core = nullptr;
	std::function<void()> stopHandler;
	LoopSource sigint;
	LoopSource sigterm;

	~Impl() {
		if (loop != nullptr) {
			sigint.reset();
			sigterm.reset();
		}
		if (core != nullptr) pw_core_disconnect(core);
		if (context != nullptr) pw_context_destroy(context);
		if (loop != nullptr) pw_main_loop_destroy(loop);
		if (initialised) {
			pw_deinit();
			pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
		}
		if (boundaryInstalled) boundary::uninstall(); // last: the teardown above may still call back
	}

	void onSignal(int /*signalNumber*/) {
		if (stopHandler) stopHandler();
	}
};

std::expected<std::unique_ptr<Engine>, EngineError> Engine::create(const EngineSettings& settings) {
	// First: every callback from here on reports to the engine's Faults, and this is the main loop's thread. One engine
	// at a time: a trampoline carries only its object, so the Faults it reports to is the process's, and a second
	// engine would take the first's (docs/architecture.md#the-callback-boundary).
	auto impl = std::make_unique<Impl>();
	if (!boundary::install(settings.faults)) {
		return std::unexpected(EngineError{"another engine is running in this process: ear runs one at a time"});
	}
	impl->boundaryInstalled = true;
	// The engine starts its data-loop thread in pw_context_new, and a stop signal sent to the
	// process goes to any thread that does not block it, where its default action ends the
	// process. So the stop signals are blocked here, before any engine thread exists, and every
	// thread started from this one later inherits that. onStopSignal reads them from a
	// signalfd, which also reports a signal that arrived before it was called.
	const sigset_t stop = stopSignals();
	pthread_sigmask(SIG_BLOCK, &stop, &impl->previousMask);
	// Before any engine thread and before the context loads module-rt.
	raisePermittedSysNice();
	pw_init(nullptr, nullptr);
	impl->initialised = true;

	impl->loop = pw_main_loop_new(nullptr);
	if (impl->loop == nullptr) {
		return std::unexpected(EngineError{"cannot make the main loop"});
	}
	impl->context = pw_context_new(pw_main_loop_get_loop(impl->loop), contextProperties(settings), 0);
	if (impl->context == nullptr) {
		return std::unexpected(
			EngineError{"cannot start the engine with " + settings.configFile.string() + ": " + spa_strerror(-errno)});
	}
	// The context's one data loop runs every process().
	pw_data_loop* dataLoop = pw_context_get_data_loop(impl->context);
	if (dataLoop == nullptr || !flushDenormalsOnDataThread(dataLoop)) {
		return std::unexpected(EngineError{"cannot flush denormals on the engine's data thread"});
	}
	impl->core = pw_context_connect_self(impl->context, nullptr, 0);
	if (impl->core == nullptr) {
		return std::unexpected(EngineError{std::string("cannot connect to the engine: ") + spa_strerror(-errno)});
	}
	return std::unique_ptr<Engine>(new Engine(std::move(impl)));
}

Engine::Engine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

namespace {
/// The engines whose data loop was judged wedged (data_loop_wedge.hpp). Main thread only.
std::set<const Engine*>& wedgedEngines() {
	static std::set<const Engine*> engines;
	return engines;
}
} // namespace

void markDataLoopWedged(const Engine& engine) {
	wedgedEngines().insert(&engine);
	boundary::markDataLoopWedged(); // every blocking step into the data loop goes through the boundary
}

bool dataLoopWedged(const Engine& engine) noexcept { return wedgedEngines().contains(&engine); }

Engine::~Engine() { wedgedEngines().erase(this); }

void Engine::run() { pw_main_loop_run(impl_->loop); }

void Engine::quit() { pw_main_loop_quit(impl_->loop); }

void Engine::onStopSignal(std::function<void()> handler) {
	impl_->stopHandler = std::move(handler);
	pw_loop* loop = pw_main_loop_get_loop(impl_->loop);
	if (!impl_->sigint) addSignal(impl_->sigint, loop, SIGINT, guarded<&Impl::onSignal>, impl_.get());
	if (!impl_->sigterm) addSignal(impl_->sigterm, loop, SIGTERM, guarded<&Impl::onSignal>, impl_.get());
}

/// The context's one data loop (engine.conf pins context.num-data-loops = 1)
/// runs every port block's process(); the question is asked on that thread itself.
Scheduling Engine::dataThreadScheduling() const {
	SchedulingRead read;
	pw_data_loop* dataLoop = pw_context_get_data_loop(impl_->context);
	if (dataLoop != nullptr) invoke(DataLoop(dataLoop), realtime<&SchedulingRead::read>, &read, true);
	return read.scheduling();
}

pw_loop* Engine::mainLoop() const noexcept { return pw_main_loop_get_loop(impl_->loop); }

pw_context* Engine::context() const noexcept { return impl_->context; }

pw_core* Engine::core() const noexcept { return impl_->core; }

} // namespace ear::pipewire
