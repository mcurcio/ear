// Inducers (header-only; the target ear_inducers puts it on the include path): the rare events a test needs, made by
// the test itself rather than waited for from the host (docs/testing.md#making-the-rare-event). A StallPlan says which
// cycles stall the data thread and for how long, Stalls carries it out from inside a process() and counts what it did,
// StallingCode is an AudioCode that does both and keeps a CycleLedger, and a CycleLedger records each cycle as the code
// saw it, with the facts its block stated, for the one judge (cycle_judge.hpp) to judge.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "ear/core/cycle_facts.hpp"
#include "ear/core/node_type.hpp"

namespace ear::test {

/// How long a planned stall holds the data thread: a fixed time, or a number of the stalled cycle's own durations. One
/// and a half cycles lets the driver's next timeout expire before the stalled node returns, so that cycle is cut short
/// straight after it, whatever the host's load (docs/engine.md#the-one-data-loop-and-invokes).
class StallLength {
public:
	static constexpr StallLength of(std::chrono::nanoseconds length) noexcept { return StallLength(length, 0.0); }
	static constexpr StallLength cycles(double count) noexcept { return StallLength({}, count); }
	/// For a cycle of `frames` at `rate`.
	constexpr std::chrono::nanoseconds at(std::uint64_t frames, std::uint32_t rate) const noexcept {
		if (cycles_ == 0.0 || rate == 0) return fixed_;
		return std::chrono::nanoseconds(static_cast<std::int64_t>(cycles_ * static_cast<double>(frames) * 1e9 / rate));
	}

private:
	constexpr StallLength(std::chrono::nanoseconds fixed, double cycles) noexcept : fixed_(fixed), cycles_(cycles) {}
	std::chrono::nanoseconds fixed_;
	double cycles_;
};

/// Which cycles stall, by an index the caller counts (a log's index, or every cycle the code ran): the cycles listed,
/// or every `every`-th from `phase` on, the first `limit` of them. A value: copy it into the code that stalls.
class StallPlan {
public:
	struct At {
		std::size_t cycle;
		StallLength length;
	};
	/// No stall.
	StallPlan() = default;
	static StallPlan at(std::vector<At> stalls) {
		StallPlan plan;
		plan.listed_ = std::move(stalls);
		return plan;
	}
	static StallPlan every(std::size_t every, std::size_t phase, std::size_t limit, StallLength length) {
		StallPlan plan;
		plan.every_ = every;
		plan.phase_ = phase;
		plan.limit_ = limit;
		plan.length_ = length;
		return plan;
	}
	/// Whether cycle `n` stalls, and for how long. Realtime: no allocation.
	std::optional<StallLength> planned(std::size_t n) const noexcept {
		if (every_ != 0 && n % every_ == phase_ && n / every_ < limit_) return length_;
		for (const At& stall : listed_)
			if (stall.cycle == n) return stall.length;
		return std::nullopt;
	}
	/// How many stalls the plan holds; an unbounded `every` (limit of the size_t's maximum) holds that many.
	std::size_t size() const noexcept { return every_ != 0 ? limit_ : listed_.size(); }

private:
	std::vector<At> listed_;
	std::size_t every_ = 0;
	std::size_t phase_ = 0;
	std::size_t limit_ = 0;
	StallLength length_ = StallLength::of({});
};

/// A plan carried out: called from inside a process() (or a probe's cycle) with that cycle's index, it sleeps on the
/// data thread when the plan says so and counts what it made. The one sleep that stalls a data thread on purpose
/// (test/sleeps.txt).
class Stalls {
public:
	explicit Stalls(StallPlan plan = {}) : plan_(std::move(plan)) {}
	/// Data thread: stalls if cycle `n` is planned (`frames` at `rate` size a stall given in cycles); true if it did.
	bool operator()(std::size_t n, std::uint64_t frames, std::uint32_t rate) noexcept {
		const std::optional<StallLength> length = plan_.planned(n);
		if (!length) return false;
		std::this_thread::sleep_for(length->at(frames, rate)); // the data thread stalls, on purpose
		made_.fetch_add(1, std::memory_order_release);
		return true;
	}
	/// Stalls made so far; any thread.
	std::size_t made() const noexcept { return made_.load(std::memory_order_acquire); }
	const StallPlan& plan() const noexcept { return plan_; }

private:
	StallPlan plan_;
	std::atomic<std::size_t> made_{0};
};

/// One cycle as a code saw it: where and when it ran, and what its block stated of it (docs/engine.md#graphcycle).
struct LedgerCycle {
	std::uint64_t position = 0;
	std::uint32_t frames = 0;
	std::int64_t startRawNs = 0;
	double rateDiff = 1.0;
	CycleFacts facts{};
	/// Nothing was lost before it: its block's own word (CycleFacts::whole), never worked out again here.
	bool whole() const noexcept { return facts.whole(); }
};

/// Each cycle a code ran, recorded on the data thread into storage made beforehand, for a CycleJudge to judge
/// (cycle_judge.hpp): the ledger works nothing out, it only keeps what the block stated.
class CycleLedger {
public:
	explicit CycleLedger(std::size_t capacity) : cycles_(capacity) {}
	CycleLedger(const CycleLedger&) = delete;
	CycleLedger& operator=(const CycleLedger&) = delete;

	/// Data thread: records a cycle and returns its index, or none when the ledger is full.
	std::optional<std::size_t> record(const LedgerCycle& cycle) noexcept {
		const std::size_t n = count_.load(std::memory_order_relaxed);
		if (n >= cycles_.size()) {
			full_.store(true, std::memory_order_release);
			return std::nullopt;
		}
		cycles_[n] = cycle;
		count_.store(n + 1, std::memory_order_release);
		return n;
	}
	/// Data thread: a block's cycle, as its code's process() was given it.
	std::optional<std::size_t> record(const GraphCycle& cycle, std::uint32_t frames) noexcept {
		return record(LedgerCycle{cycle.position, frames, cycle.startRawNs, cycle.rateDiff, cycle.facts});
	}

	/// Any thread: cycles recorded.
	std::size_t count() const noexcept { return count_.load(std::memory_order_acquire); }
	bool full() const noexcept { return count() == cycles_.size(); }
	/// A cycle was offered when the ledger was full: it holds less than the code ran.
	bool overflowed() const noexcept { return full_.load(std::memory_order_acquire); }
	std::size_t capacity() const noexcept { return cycles_.size(); }
	/// Cycle `i`, below count() as read on this thread.
	const LedgerCycle& operator[](std::size_t i) const noexcept { return cycles_[i]; }
	/// The cycles recorded so far.
	std::span<const LedgerCycle> cycles() const noexcept { return {cycles_.data(), count()}; }

private:
	std::vector<LedgerCycle> cycles_;
	std::atomic<std::size_t> count_{0};
	std::atomic<bool> full_{false};
};

/// A block's code that writes silence, records each cycle it runs into its ledger (until it is full) and stalls the
/// data thread at the cycles its plan names (by ledger index), then calls `wake` (the test's Waiter) every cycle.
class StallingCode final : public AudioCode {
public:
	StallingCode(std::size_t capacity, StallPlan plan, std::function<void()> wake = {})
		: ledger_(capacity), stalls_(std::move(plan)), wake_(std::move(wake)) {}
	void prepare(const AudioFormat& format) override { rate_ = format.rate; }
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle& cycle) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
		if (const std::optional<std::size_t> n = ledger_.record(cycle, frames)) stalls_(*n, frames, rate_);
		if (wake_) wake_();
	}
	void release() noexcept override {}

	const CycleLedger& ledger() const noexcept { return ledger_; }
	/// Stalls made so far.
	std::size_t stalls() const noexcept { return stalls_.made(); }

private:
	CycleLedger ledger_;
	Stalls stalls_;
	std::function<void()> wake_;
	std::uint32_t rate_ = 0;
};

} // namespace ear::test
