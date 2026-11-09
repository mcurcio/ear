// The main-loop watchdog (docs/design/node-lifecycle.md section 5.12, D-L9, step L-7; node-framework.md 6.3 and 6.8):
// the main loop beats a heartbeat (main.cpp: a 100 ms main-loop timer), and a thread of its own looks at it every
// 250 ms. A look that finds the heartbeat older than 500 ms reports the stall once, with the heartbeat's age and where
// the main loop was (a callback main.cpp gives: ear_output sees nothing of EAR's); once the beats return it reports the
// gap between the beats either side, which the first beat after the stall measures itself. Looks are a period apart, so
// any stall longer than bound + period is reported, and one between bound and bound + period may not be. It reads two
// atomics, takes no lock the main loop holds and never touches PipeWire; what it reports goes straight to the output
// queue, which never waits on a reader, so a stall is visible while it lasts.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
namespace ear::output {
/// The watchdog's judgement, without a thread or a clock of its own: told the last beat, the last long gap a beat
/// measured and the time at each look, it says when a stall begins and when it ends. Armed by the first beat, so a
/// boot that has not reached the main loop yet is never a stall. Times are nanoseconds on one monotonic clock; a beat
/// of 0 is none yet.
class StallJudge {
public:
	struct Verdict {
		enum class Kind { stalled, resumed } kind;
		/// stalled: the heartbeat's age at the look that saw the stall; resumed: the stall's length, from the last beat
		/// before it to the first after it.
		std::int64_t ms;
	};
	/// The last gap between two beats longer than the bound, as the later beat recorded it: the beat before (`from`)
	/// and its own time (`to`); {0, 0} when none.
	struct Gap {
		std::int64_t from = 0;
		std::int64_t to = 0;
	};
	explicit StallJudge(std::chrono::nanoseconds bound) : bound_(bound.count()) {}
	/// One look at `beat` (the last beat's time) at `now`, with the last long `gap`. A resumption's length is the
	/// gap that starts at the stall's last beat; without one (a gap not yet seen), it is the distance to `beat`, which
	/// may be later than the first beat after the stall.
	std::optional<Verdict> look(std::int64_t beat, std::int64_t now, Gap gap);
	std::optional<Verdict> look(std::int64_t beat, std::int64_t now) { return look(beat, now, Gap{}); }
	bool stalled() const { return stalled_; }

private:
	std::int64_t bound_;
	bool stalled_ = false;
	std::int64_t stalledAt_ = 0; // the last beat before the stall
};

class StallWatch {
public:
	struct Options {
		/// A heartbeat older than this is a stall.
		std::chrono::milliseconds bound{500};
		/// How often the thread looks.
		std::chrono::milliseconds period{250};
	};
	/// Where the main loop is, called on the watchdog's thread when it reports a stall; empty when unknown.
	using Where = std::function<std::string()>;
	/// The watchdog's thread: a stall past the bound, once per stall (`ms`, the heartbeat's age; `where`).
	using Stalled = std::function<void(std::int64_t ms, const std::string& where)>;
	/// The watchdog's thread: the beats came back after a stall it reported (`ms`, the gap between the beats either
	/// side).
	using Resumed = std::function<void(std::int64_t ms)>;

	/// Starts the thread (every signal but the faults blocked). Nothing is judged until the first beat().
	StallWatch(Where where, Stalled stalled, Resumed resumed, Options options);
	StallWatch(Where where, Stalled stalled, Resumed resumed)
		: StallWatch(std::move(where), std::move(stalled), std::move(resumed), Options{}) {}
	~StallWatch(); // stop()
	StallWatch(const StallWatch&) = delete;
	StallWatch& operator=(const StallWatch&) = delete;

	/// One thread only (the main loop): a beat, now. Never waits: an atomic load and one or two stores. A beat that
	/// follows the last by more than the bound records the gap for the watchdog.
	void beat() noexcept;
	/// Ends the thread and joins it, reporting nothing more (so ear's stop, past the main loop, is never a stall).
	/// Idempotent.
	void stop();

private:
	void run();

	Where where_;
	Stalled stalled_;
	Resumed resumed_;
	Options options_;
	std::atomic<std::int64_t> heartbeat_{0};
	// The last long gap (StallJudge::Gap), written by the beat that ends it before heartbeat_, so a look that sees that
	// beat sees its gap.
	std::atomic<std::int64_t> gapFrom_{0};
	std::atomic<std::int64_t> gapTo_{0};
	std::mutex mutex_;
	std::condition_variable wake_;
	bool stopping_ = false; // under mutex_
	std::thread thread_;
};
} // namespace ear::output
