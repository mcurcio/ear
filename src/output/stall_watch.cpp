#include "ear/output/stall_watch.hpp"

#include <utility>

#include "masked_thread.hpp"

namespace ear::output {

namespace {

constexpr std::int64_t nanosPerMs = 1'000'000;

/// The steady clock in nanoseconds, never 0 (0 is "no beat yet").
std::int64_t steadyNow() noexcept {
	const auto ns =
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
			.count();
	return ns == 0 ? 1 : ns;
}

} // namespace

std::optional<StallJudge::Verdict> StallJudge::look(std::int64_t beat, std::int64_t now) {
	if (beat == 0) return std::nullopt; // not armed: the main loop has not run yet
	if (stalled_) {
		if (beat == stalledAt_) return std::nullopt;
		stalled_ = false;
		return Verdict{Verdict::Kind::resumed, (beat - stalledAt_) / nanosPerMs};
	}
	if (now - beat <= bound_) return std::nullopt;
	stalled_ = true;
	stalledAt_ = beat;
	return Verdict{Verdict::Kind::stalled, (now - beat) / nanosPerMs};
}

StallWatch::StallWatch(Where where, Stalled stalled, Resumed resumed, Options options)
	: where_(std::move(where)), stalled_(std::move(stalled)), resumed_(std::move(resumed)), options_(options),
	  thread_(maskedThread([this] { run(); })) {}

StallWatch::~StallWatch() { stop(); }

void StallWatch::beat() noexcept { heartbeat_.store(steadyNow(), std::memory_order_relaxed); }

void StallWatch::stop() {
	{
		const std::scoped_lock lock(mutex_);
		stopping_ = true;
	}
	wake_.notify_all();
	if (thread_.joinable()) thread_.join();
}

void StallWatch::run() {
	StallJudge judge(options_.bound);
	std::unique_lock lock(mutex_);
	while (!wake_.wait_for(lock, options_.period, [this] { return stopping_; })) {
		lock.unlock();
		const auto verdict = judge.look(heartbeat_.load(std::memory_order_relaxed), steadyNow());
		if (verdict && verdict->kind == StallJudge::Verdict::Kind::stalled) stalled_(verdict->ms, where_());
		else if (verdict) resumed_(verdict->ms);
		lock.lock();
	}
}

} // namespace ear::output
