// The nodes' loops on real threads (ear/runtime/loops.hpp). One rule for both kinds: a loop is a Core, its queue of
// work (posts, and fires of its timers and watches), run one item at a time; its timers and watches live in a Reactor
// (epoll, an eventfd to interrupt it, a timer heap) that turns each due timer or readable descriptor into a fire on its
// loop's queue. A dedicated loop's thread turns its own reactor and runs its queue between turns; strands share the
// pool's reactor thread, which also watches the workers, and the pool's workers run each strand's queue one item at a
// time, a strand on one worker at a time.
#include "ear/runtime/loops.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <list>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace ear::runtime {
namespace {

using Clock = std::chrono::steady_clock;

class Core;

/// A timer or a watch, armed on one loop.
struct Source {
	enum class Kind { once, every, wake, readable };
	Kind kind;
	std::uint64_t id;
	std::weak_ptr<Core> owner{};
	std::move_only_function<void()> handler{};
	Clock::time_point due{};  // a timer's: the reactor's after it was armed
	Clock::duration period{}; // every's
	int fd = -1;              // a watch's
	Wake* wake = nullptr;     // onWake's
	bool cancelled = false;   // its owner's mutex
	bool queued = false;      // its owner's mutex: a fire of it waits in its owner's queue
	bool isWatch() const noexcept { return kind == Kind::wake || kind == Kind::readable; }
};

std::atomic<std::uint64_t> sourceIds{1}; // 0 is a reactor's own eventfd

std::system_error lastError(const char* what) { return {errno, std::generic_category(), what}; }

/// What the exception being handled said.
std::string thrownWhat() {
	try {
		throw;
	} catch (const std::exception& error) {
		return error.what();
	} catch (...) {
		return "an exception that is not a std::exception";
	}
}

/// Timers and watches: each due timer and readable descriptor is handed to its loop as a fire (Core::fire). Watches are
/// one-shot in epoll and re-armed after their handler ran, so a descriptor is never handed twice before it ran.
class Reactor {
public:
	Reactor() : epoll_(::epoll_create1(EPOLL_CLOEXEC)), bell_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
		if (epoll_ < 0 || bell_ < 0) {
			const std::system_error error = lastError("cannot make a loop's reactor");
			closeAll();
			throw error;
		}
		epoll_event bell{.events = EPOLLIN, .data = {.u64 = 0}};
		if (::epoll_ctl(epoll_, EPOLL_CTL_ADD, bell_, &bell) != 0) {
			const std::system_error error = lastError("cannot watch a loop's reactor bell");
			closeAll();
			throw error;
		}
	}
	~Reactor() { closeAll(); }
	Reactor(const Reactor&) = delete;
	Reactor& operator=(const Reactor&) = delete;

	/// Any thread: arms `source`; a watch whose descriptor epoll refuses throws std::system_error.
	void add(const std::shared_ptr<Source>& source) {
		{
			const std::scoped_lock lock(mutex_);
			sources_[source->id] = source;
			if (!source->isWatch()) timers_.push(Due{source->due, source->id});
		}
		if (source->isWatch()) {
			epoll_event watched{.events = EPOLLIN | EPOLLONESHOT, .data = {.u64 = source->id}};
			if (::epoll_ctl(epoll_, EPOLL_CTL_ADD, source->fd, &watched) != 0) {
				const std::system_error error = lastError("cannot watch a descriptor on a loop");
				const std::scoped_lock lock(mutex_);
				sources_.erase(source->id);
				throw error;
			}
		}
		interrupt();
	}
	/// After a watch's handler ran: watched again.
	void rearm(const Source& source) noexcept {
		epoll_event watched{.events = EPOLLIN | EPOLLONESHOT, .data = {.u64 = source.id}};
		static_cast<void>(::epoll_ctl(epoll_, EPOLL_CTL_MOD, source.fd, &watched));
	}
	/// Any thread: never fired again.
	void remove(const Source& source) noexcept {
		{
			const std::scoped_lock lock(mutex_);
			sources_.erase(source.id);
		}
		if (source.isWatch()) static_cast<void>(::epoll_ctl(epoll_, EPOLL_CTL_DEL, source.fd, nullptr));
	}
	void interrupt() noexcept {
		const std::uint64_t one = 1;
		[[maybe_unused]] const auto ignored = ::write(bell_, &one, sizeof one);
	}
	/// Waits up to `maxWait` (none: until a timer is due, a descriptor readable or interrupt()), then fires what is due.
	void turn(std::optional<Clock::duration> maxWait) {
		int timeout = -1;
		{
			const std::scoped_lock lock(mutex_);
			std::optional<Clock::duration> wait = maxWait;
			if (!timers_.empty()) {
				const Clock::duration untilDue = std::max(Clock::duration::zero(), timers_.top().at - Clock::now());
				wait = wait ? std::min(*wait, untilDue) : untilDue;
			}
			if (wait)
				timeout = static_cast<int>(std::min<std::int64_t>(
					std::chrono::ceil<std::chrono::milliseconds>(*wait).count(), std::int64_t{60'000}));
		}
		std::array<epoll_event, 32> events{};
		const int ready = ::epoll_wait(epoll_, events.data(), static_cast<int>(events.size()), timeout);
		std::vector<std::shared_ptr<Source>> fired;
		{
			const std::scoped_lock lock(mutex_);
			for (int i = 0; i < ready; ++i) {
				const std::uint64_t id = events[static_cast<std::size_t>(i)].data.u64;
				if (id == 0) {
					std::uint64_t count = 0;
					[[maybe_unused]] const auto ignored = ::read(bell_, &count, sizeof count);
					continue;
				}
				if (const auto found = sources_.find(id); found != sources_.end())
					if (auto source = found->second.lock()) fired.push_back(std::move(source));
			}
			const Clock::time_point now = Clock::now();
			while (!timers_.empty() && timers_.top().at <= now) {
				const Due due = timers_.top();
				timers_.pop();
				const auto found = sources_.find(due.id);
				if (found == sources_.end()) continue; // cancelled
				auto source = found->second.lock();
				if (!source || source->due != due.at) continue;
				if (source->kind == Source::Kind::every) {
					// Fixed rate: the next tick after now; ticks missed meanwhile are one.
					const auto missed = (now - source->due) / source->period;
					source->due += source->period * (missed + 1);
					timers_.push(Due{source->due, source->id});
				} else {
					sources_.erase(found);
				}
				fired.push_back(std::move(source));
			}
		}
		for (auto& source : fired) fire(source);
	}

private:
	struct Due {
		Clock::time_point at;
		std::uint64_t id;
		bool operator>(const Due& other) const noexcept { return at > other.at; }
	};
	static void fire(const std::shared_ptr<Source>& source);
	void closeAll() noexcept {
		if (epoll_ >= 0) ::close(epoll_);
		if (bell_ >= 0) ::close(bell_);
	}
	int epoll_;
	int bell_;
	std::mutex mutex_;
	std::unordered_map<std::uint64_t, std::weak_ptr<Source>> sources_;  // mutex_
	std::priority_queue<Due, std::vector<Due>, std::greater<>> timers_; // mutex_
};

thread_local const Core* runningHere = nullptr; // the loop whose handler this thread runs

/// One loop: its queue, its sources, and the rules of the node's intake, outside work, finish and close.
class Core final : public std::enable_shared_from_this<Core> {
public:
	enum class Turned { idle, more, exit };

	Core(LoopKind kind, Executor& mainLoop, LoopEvents& events, std::shared_ptr<Reactor> reactor,
	     std::chrono::milliseconds slowBound)
		: kind_(kind), mainLoop_(mainLoop), events_(&events), reactor_(std::move(reactor)), slowBound_(slowBound) {}

	/// How a loop asks to be run once its queue holds work: a dedicated loop's thread is interrupted, a strand joins the
	/// pool's ready queue. Unset: no runner (a thread that never started), so finish and close answer at once.
	void runBy(std::move_only_function<void(std::shared_ptr<Core>)> runner) { runner_ = std::move(runner); }

	LoopKind kind() const noexcept { return kind_; }
	bool inLoop() const noexcept { return runningHere == this; }

	void post(std::move_only_function<void()> work, bool outside = false) {
		std::move_only_function<void()> refused;
		bool schedule = false;
		{
			const std::scoped_lock lock(mutex_);
			if (closing_ || (outside && othersClosed_)) refused = std::move(work); // destroyed below, unrun
			else schedule = pushLocked(Item{.work = std::move(work), .outside = outside});
		}
		if (schedule) runner_(shared_from_this());
	}

	std::unique_ptr<Timer> arm(std::shared_ptr<Source> source);

	/// A source's handle went: never fired again.
	void cancel(const std::shared_ptr<Source>& source) noexcept {
		{
			const std::scoped_lock lock(mutex_);
			source->cancelled = true;
		}
		if (reactor_) reactor_->remove(*source);
	}

	/// The reactor: a fire of `source` queued, unless one waits already (ticks coalesce) or it was cancelled.
	void fire(const std::shared_ptr<Source>& source) {
		bool schedule = false;
		{
			const std::scoped_lock lock(mutex_);
			if (source->cancelled || source->queued || sourcesCancelled_) return;
			source->queued = true;
			schedule = pushLocked(Item{.fire = source});
		}
		if (schedule) runner_(shared_from_this());
	}

	void closeToOthers() noexcept {
		const std::scoped_lock lock(mutex_);
		othersClosed_ = true; // queued outside work is destroyed unrun as it comes up, on the loop's thread
	}

	void finish(std::move_only_function<void()> last, std::move_only_function<void()> finished) {
		if (!runner_) { // no thread: nothing runs, so the state goes here
			last();
			report(std::move(finished));
			return;
		}
		bool schedule = false;
		{
			const std::scoped_lock lock(mutex_);
			schedule =
				pushLocked(Item{.work = [this, last = std::move(last), finished = std::move(finished)]() mutable {
					last();
					report(std::move(finished));
				}});
		}
		if (schedule) runner_(shared_from_this());
	}

	void close(std::move_only_function<void()> closed) {
		bool schedule = false;
		{
			const std::scoped_lock lock(mutex_);
			closing_ = true;
			if (runner_) schedule = pushLocked(Item{.closed = std::move(closed), .closeMark = true});
		}
		if (!runner_) {
			cancelSources();
			report(std::move(closed));
			return;
		}
		if (schedule) runner_(shared_from_this());
	}

	/// Main thread, as its NodeLoop goes: nothing more is posted to the main loop for it.
	void retire() noexcept {
		const std::scoped_lock lock(mutex_);
		token_.reset();
	}
	bool closeAsked() const noexcept {
		const std::scoped_lock lock(mutex_);
		return closing_;
	}
	bool hasWork() const noexcept {
		const std::scoped_lock lock(mutex_);
		return !queue_.empty();
	}

	/// The loop's thread (its own, or a pool worker): runs the next item.
	Turned runOne();

	/// Posts `told` to the main loop, unless the loop's NodeLoop went first.
	void report(std::move_only_function<void()> told) {
		std::weak_ptr<int> alive;
		{
			const std::scoped_lock lock(mutex_);
			alive = token_;
		}
		mainLoop_.post([alive = std::move(alive), told = std::move(told)]() mutable {
			if (alive.lock()) told();
		});
	}
	/// Posts an event to the loop's LoopEvents on the main loop.
	void tell(std::move_only_function<void(LoopEvents&)> event) {
		report([events = events_, event = std::move(event)]() mutable { event(*events); });
	}

private:
	struct Item {
		std::move_only_function<void()> work{};
		bool outside = false;
		std::shared_ptr<Source> fire{};
		std::move_only_function<void()> closed{};
		bool closeMark = false;
	};
	/// mutex_: queued; true when the loop must now be asked to run.
	bool pushLocked(Item item) {
		queue_.push_back(std::move(item));
		return !std::exchange(scheduled_, true);
	}
	void runFire(const std::shared_ptr<Source>& source) {
		{
			const std::scoped_lock lock(mutex_);
			if (source->cancelled) return;
			source->queued = false;
		}
		if (source->kind == Source::Kind::wake) source->wake->answer(); // drained, then cleared, before the handler
		source->handler();
		if (!source->isWatch()) return;
		bool live = false;
		{
			const std::scoped_lock lock(mutex_);
			live = !source->cancelled;
		}
		if (live) reactor_->rearm(*source);
	}
	/// Every source left: cancelled and no longer fired.
	void cancelSources() noexcept {
		std::vector<std::shared_ptr<Source>> left;
		{
			const std::scoped_lock lock(mutex_);
			sourcesCancelled_ = true;
			for (auto& weak : live_)
				if (auto source = weak.lock(); source && !source->cancelled) {
					source->cancelled = true;
					left.push_back(std::move(source));
				}
			live_.clear();
		}
		if (reactor_)
			for (const auto& source : left) reactor_->remove(*source);
	}

	const LoopKind kind_;
	Executor& mainLoop_;
	LoopEvents* events_;
	std::shared_ptr<Reactor> reactor_;
	const std::chrono::milliseconds slowBound_;
	std::move_only_function<void(std::shared_ptr<Core>)> runner_;
	mutable std::mutex mutex_;
	std::deque<Item> queue_;                                // mutex_
	std::vector<std::weak_ptr<Source>> live_;               // mutex_: every source armed, for the close
	bool scheduled_ = false;                                // mutex_: the loop was asked to run and has work
	bool closing_ = false;                                  // mutex_: close() was called: no more work
	bool othersClosed_ = false;                             // mutex_
	bool sourcesCancelled_ = false;                         // mutex_: the close ran
	std::shared_ptr<int> token_ = std::make_shared<int>(0); // mutex_; reset by retire()
	bool slowSaid_ = false;                                 // the loop's thread only
};

/// What arming a source returns: its destruction cancels it.
class SourceHandle final : public Timer {
public:
	SourceHandle(std::shared_ptr<Core> core, std::shared_ptr<Source> source)
		: core_(std::move(core)), source_(std::move(source)) {}
	~SourceHandle() override { core_->cancel(source_); }

private:
	std::shared_ptr<Core> core_;
	std::shared_ptr<Source> source_;
};

void Reactor::fire(const std::shared_ptr<Source>& source) {
	if (auto core = source->owner.lock()) core->fire(source);
}

std::unique_ptr<Timer> Core::arm(std::shared_ptr<Source> source) {
	source->owner = weak_from_this();
	bool cancelled = false;
	{
		const std::scoped_lock lock(mutex_);
		cancelled = sourcesCancelled_ || !reactor_;
		source->cancelled = cancelled;
		if (!cancelled) {
			std::erase_if(live_, [](const std::weak_ptr<Source>& weak) { return weak.expired(); });
			live_.push_back(source);
		}
	}
	if (!cancelled) reactor_->add(source);
	return std::make_unique<SourceHandle>(shared_from_this(), std::move(source));
}

Core::Turned Core::runOne() {
	Item item;
	{
		const std::scoped_lock lock(mutex_);
		if (queue_.empty()) {
			scheduled_ = false;
			return Turned::idle;
		}
		item = std::move(queue_.front());
		queue_.pop_front();
	}
	bool exit = false;
	{
		const Core* outer = std::exchange(runningHere, this);
		std::optional<StrandScope> strand;
		if (kind_ == LoopKind::strand) strand.emplace();
		const Clock::time_point began = Clock::now();
		bool othersClosed = false;
		{
			const std::scoped_lock lock(mutex_);
			othersClosed = othersClosed_;
		}
		try {
			if (item.closeMark) {
				cancelSources();
				report(std::move(item.closed));
				exit = true;
			} else if (item.fire) {
				runFire(item.fire);
			} else if (!(item.outside && othersClosed)) {
				item.work();
			}
		} catch (...) {
			tell([what = thrownWhat()](LoopEvents& events) mutable { events.threw(std::move(what)); });
		}
		const auto ran = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began);
		if (kind_ == LoopKind::strand && ran > slowBound_ && !std::exchange(slowSaid_, true))
			tell([ms = ran.count()](LoopEvents& events) { events.slow(ms); });
		item = {}; // what the work held goes on the loop's thread
		runningHere = outer;
	}
	std::deque<Item> leftover;
	Turned turned = Turned::more;
	{
		const std::scoped_lock lock(mutex_);
		if (exit) leftover.swap(queue_); // only fires of cancelled sources can follow the close
		if (queue_.empty()) {
			scheduled_ = false;
			turned = exit ? Turned::exit : Turned::idle;
		}
	}
	return turned;
}

/// A dedicated loop: its own reactor and thread.
class DedicatedLoop final : public NodeLoop {
public:
	DedicatedLoop(std::shared_ptr<Core> core, std::shared_ptr<Reactor> reactor)
		: core_(std::move(core)), reactor_(std::move(reactor)) {}
	~DedicatedLoop() override {
		core_->retire();
		if (!core_->closeAsked()) core_->close([] {}); // never closed (a configuration that never started it)
		if (thread_.joinable()) thread_.join();
	}
	/// Starts the thread; std::system_error when it cannot.
	void start(const std::string& name) {
		core_->runBy([reactor = reactor_](std::shared_ptr<Core>) { reactor->interrupt(); });
		thread_ = std::thread([core = core_, reactor = reactor_] {
			for (;;) {
				reactor->turn(core->hasWork() ? std::optional<Clock::duration>(Clock::duration::zero()) : std::nullopt);
				if (core->runOne() == Core::Turned::exit) return;
			}
		});
		// The name ps and a debugger show (at most 15 bytes); a failure only leaves the process's name.
		::pthread_setname_np(thread_.native_handle(), name.c_str());
	}
	/// No thread could start: the loop answers finish and close at once.
	void noThread() { core_->runBy(nullptr); }

	void post(std::move_only_function<void()> work) override { core_->post(std::move(work)); }
	std::unique_ptr<Timer> after(std::chrono::milliseconds delay, std::move_only_function<void()> due) override {
		return arm(Source::Kind::once, delay, std::move(due));
	}
	std::unique_ptr<Timer> every(std::chrono::milliseconds period, std::move_only_function<void()> due) override {
		return arm(Source::Kind::every, std::max(period, std::chrono::milliseconds{1}), std::move(due));
	}
	std::unique_ptr<Timer> onWake(Wake& wake, std::move_only_function<void()> rung) override {
		auto source = std::make_shared<Source>(Source{.kind = Source::Kind::wake,
		                                              .id = sourceIds++,
		                                              .handler = std::move(rung),
		                                              .fd = wake.descriptor(),
		                                              .wake = &wake});
		return core_->arm(std::move(source));
	}
	std::unique_ptr<Timer> onReadable(int fd, std::move_only_function<void()> readable) override {
		auto source = std::make_shared<Source>(
			Source{.kind = Source::Kind::readable, .id = sourceIds++, .handler = std::move(readable), .fd = fd});
		return core_->arm(std::move(source));
	}
	LoopKind kind() const noexcept override { return core_->kind(); }
	bool inLoop() const noexcept override { return core_->inLoop(); }
	void postFromOutside(std::move_only_function<void()> work) override { core_->post(std::move(work), true); }
	void closeToOthers() override { core_->closeToOthers(); }
	void finish(std::move_only_function<void()> last, std::move_only_function<void()> finished) override {
		core_->finish(std::move(last), std::move(finished));
	}
	void close(std::move_only_function<void()> closed) override { core_->close(std::move(closed)); }

private:
	std::unique_ptr<Timer> arm(Source::Kind kind, std::chrono::milliseconds delay,
	                           std::move_only_function<void()> due) {
		auto source = std::make_shared<Source>(Source{
			.kind = kind, .id = sourceIds++, .handler = std::move(due), .due = Clock::now() + delay, .period = delay});
		return core_->arm(std::move(source));
	}
	std::shared_ptr<Core> core_;
	std::shared_ptr<Reactor> reactor_;
	std::thread thread_;
};

/// The strand pool: workers that run strands' queues, and one reactor thread that turns the strands' timers and watches
/// into fires and watches the workers (a handler past replaceAfter gets a replacement worker).
class StrandPool : public std::enable_shared_from_this<StrandPool> {
public:
	explicit StrandPool(const LoopOptions& o) : options(o), reactor(std::make_shared<Reactor>()) {}
	~StrandPool() { stop(); }

	struct Worker {
		std::thread thread;
		bool busy = false;         // mutex
		Clock::time_point since{}; // mutex: when its handler began
		bool replaced = false;     // mutex: a replacement was started for its handler
		bool exited = false;       // mutex
	};

	/// Starts the base workers and the reactor thread; std::system_error when none could start.
	void start() {
		base = options.strandThreads != 0 ? options.strandThreads
		                                  : std::min<std::size_t>(4, std::max(1U, std::thread::hardware_concurrency()));
		const std::scoped_lock lock(mutex);
		for (std::size_t i = 0; i < base; ++i) spawnLocked();
		reactorThread = std::thread([this] { reactorLoop(); });
		::pthread_setname_np(reactorThread.native_handle(), "ear-strands");
	}
	void stop() {
		{
			const std::scoped_lock lock(mutex);
			if (stopping) return;
			stopping = true;
		}
		changed.notify_all();
		reactor->interrupt();
		if (reactorThread.joinable()) reactorThread.join();
		std::list<Worker> joined;
		{
			const std::scoped_lock lock(mutex);
			joined.swap(workers);
		}
		for (Worker& worker : joined)
			if (worker.thread.joinable()) worker.thread.join();
	}
	void schedule(std::shared_ptr<Core> core) {
		{
			const std::scoped_lock lock(mutex);
			ready.push_back(std::move(core));
		}
		changed.notify_one();
	}
	std::size_t threads() const {
		const std::scoped_lock lock(mutex);
		return live;
	}

	/// mutex: one more worker.
	void spawnLocked() {
		Worker& worker = workers.emplace_back();
		worker.thread = std::thread([this, &worker] { work(worker); });
		::pthread_setname_np(worker.thread.native_handle(), "ear-strand");
		++live;
	}
	void work(Worker& self) {
		std::unique_lock lock(mutex);
		for (;;) {
			changed.wait(lock, [&] { return stopping || !ready.empty(); });
			if (stopping) break;
			std::shared_ptr<Core> core = std::move(ready.front());
			ready.pop_front();
			self.busy = true;
			self.since = Clock::now();
			const bool first = busyCount++ == 0;
			lock.unlock();
			if (first) reactor->interrupt(); // the reactor watches the workers while any is busy
			const Core::Turned turned = core->runOne();
			lock.lock();
			self.busy = false;
			--busyCount;
			if (turned == Core::Turned::more) {
				ready.push_back(std::move(core));
				changed.notify_one();
			}
			if (self.replaced && live > base) break; // its replacement carries on
			self.replaced = false;
		}
		self.exited = true;
		--live;
	}
	/// Every 50 ms while a worker is busy: one past replaceAfter gets a replacement, within maxThreads.
	bool supervise() {
		std::list<Worker> exited;
		bool busy = false;
		{
			const std::scoped_lock lock(mutex);
			const Clock::time_point now = Clock::now();
			for (Worker& worker : workers)
				if (worker.busy && !worker.replaced && now - worker.since > options.replaceAfter) {
					worker.replaced = true;
					if (live < options.maxThreads) spawnLocked();
				}
			// A worker that left (its replacement carries on) is joined here, so threads never pile up: it set `exited`
			// as its last act under the lock, so the join waits only for its return.
			for (auto it = workers.begin(); it != workers.end();) {
				auto next = std::next(it);
				if (it->exited) exited.splice(exited.end(), workers, it);
				it = next;
			}
			busy = busyCount > 0;
		}
		for (Worker& worker : exited)
			if (worker.thread.joinable()) worker.thread.join();
		return busy;
	}
	/// Threads the pool still holds, running or not yet joined.
	std::size_t held() const {
		const std::scoped_lock lock(mutex);
		return workers.size();
	}
	void reactorLoop() {
		for (;;) {
			{
				const std::scoped_lock lock(mutex);
				if (stopping) return;
			}
			const bool watching = supervise();
			reactor->turn(watching ? std::optional<Clock::duration>(std::chrono::milliseconds{50}) : std::nullopt);
		}
	}

	const LoopOptions options;
	std::shared_ptr<Reactor> reactor;
	mutable std::mutex mutex;
	std::condition_variable changed;
	std::deque<std::shared_ptr<Core>> ready; // mutex
	std::list<Worker> workers;               // mutex: stable addresses
	std::size_t base = 0;
	std::size_t live = 0;      // mutex: workers not exited
	std::size_t busyCount = 0; // mutex
	bool stopping = false;     // mutex
	std::thread reactorThread;
};

/// A strand: its queue run on the pool's workers, its sources on the pool's reactor.
class Strand final : public NodeLoop {
public:
	Strand(std::shared_ptr<Core> core, std::shared_ptr<StrandPool> pool)
		: core_(std::move(core)), pool_(std::move(pool)) {}
	~Strand() override {
		core_->retire();
		if (!core_->closeAsked()) core_->close([] {});
	}
	void post(std::move_only_function<void()> work) override { core_->post(std::move(work)); }
	std::unique_ptr<Timer> after(std::chrono::milliseconds delay, std::move_only_function<void()> due) override {
		return arm(Source::Kind::once, delay, std::move(due));
	}
	std::unique_ptr<Timer> every(std::chrono::milliseconds period, std::move_only_function<void()> due) override {
		return arm(Source::Kind::every, std::max(period, std::chrono::milliseconds{1}), std::move(due));
	}
	std::unique_ptr<Timer> onWake(Wake& wake, std::move_only_function<void()> rung) override {
		return core_->arm(std::make_shared<Source>(Source{.kind = Source::Kind::wake,
		                                                  .id = sourceIds++,
		                                                  .handler = std::move(rung),
		                                                  .fd = wake.descriptor(),
		                                                  .wake = &wake}));
	}
	std::unique_ptr<Timer> onReadable(int fd, std::move_only_function<void()> readable) override {
		return core_->arm(std::make_shared<Source>(
			Source{.kind = Source::Kind::readable, .id = sourceIds++, .handler = std::move(readable), .fd = fd}));
	}
	LoopKind kind() const noexcept override { return core_->kind(); }
	bool inLoop() const noexcept override { return core_->inLoop(); }
	void postFromOutside(std::move_only_function<void()> work) override { core_->post(std::move(work), true); }
	void closeToOthers() override { core_->closeToOthers(); }
	void finish(std::move_only_function<void()> last, std::move_only_function<void()> finished) override {
		core_->finish(std::move(last), std::move(finished));
	}
	void close(std::move_only_function<void()> closed) override { core_->close(std::move(closed)); }

private:
	std::unique_ptr<Timer> arm(Source::Kind kind, std::chrono::milliseconds delay,
	                           std::move_only_function<void()> due) {
		return core_->arm(std::make_shared<Source>(Source{
			.kind = kind, .id = sourceIds++, .handler = std::move(due), .due = Clock::now() + delay, .period = delay}));
	}
	std::shared_ptr<Core> core_;
	std::shared_ptr<StrandPool> pool_; // its workers and reactor outlive the strand
};

/// The pool `held` keeps.
StrandPool* poolOf(const std::shared_ptr<void>& held) noexcept { return static_cast<StrandPool*>(held.get()); }

} // namespace

Loops::Loops(Executor& mainLoop, LoopOptions options) : mainLoop_(mainLoop), options_(options) {}

Loops::~Loops() {
	if (pool_) poolOf(pool_)->stop();
}

std::unique_ptr<NodeLoop> Loops::make(const NodeName& node, std::string_view name, LoopKind kind, LoopEvents& events) {
	if (kind == LoopKind::dedicated) {
		std::shared_ptr<Reactor> reactor;
		try {
			reactor = std::make_shared<Reactor>();
		} catch (const std::system_error& error) {
			// No reactor, so no loop: one that answers finish and close at once, its state destroyed where they run.
			auto core = std::make_shared<Core>(kind, mainLoop_, events, nullptr, options_.slowBound);
			auto loop = std::make_unique<DedicatedLoop>(core, nullptr);
			loop->noThread();
			core->tell([why = error.code().message()](LoopEvents& told) mutable { told.failed(std::move(why)); });
			return loop;
		}
		auto core = std::make_shared<Core>(kind, mainLoop_, events, reactor, options_.slowBound);
		auto loop = std::make_unique<DedicatedLoop>(core, reactor);
		try {
			loop->start(loopThreadName(name, node));
		} catch (const std::system_error& error) {
			loop->noThread();
			core->tell([why = error.code().message()](LoopEvents& told) mutable { told.failed(std::move(why)); });
			return loop;
		}
		core->tell([](LoopEvents& told) { told.ready(); });
		return loop;
	}
	if (!pool_) {
		try {
			auto pool = std::make_shared<StrandPool>(options_);
			pool->start();
			pool_ = std::move(pool);
		} catch (const std::system_error& error) {
			auto core = std::make_shared<Core>(kind, mainLoop_, events, nullptr, options_.slowBound);
			core->tell([why = error.code().message()](LoopEvents& told) mutable { told.failed(std::move(why)); });
			return std::make_unique<Strand>(std::move(core), nullptr);
		}
	}
	auto pool = std::static_pointer_cast<StrandPool>(pool_);
	auto core = std::make_shared<Core>(kind, mainLoop_, events, pool->reactor, options_.slowBound);
	core->runBy([weak = std::weak_ptr<StrandPool>(pool)](std::shared_ptr<Core> ready) {
		if (auto alive = weak.lock()) alive->schedule(std::move(ready));
	});
	core->tell([](LoopEvents& told) { told.ready(); });
	return std::make_unique<Strand>(std::move(core), std::move(pool));
}

std::size_t Loops::poolThreads() const { return pool_ ? poolOf(pool_)->threads() : 0; }

std::size_t Loops::poolThreadsHeld() const { return pool_ ? poolOf(pool_)->held() : 0; }

} // namespace ear::runtime
