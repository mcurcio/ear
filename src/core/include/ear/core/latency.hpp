#pragma once
// ear's one latency model (docs/engine.md#latency-per-bank-pair): the only code that combines latency or turns frames
// into time. Pure: no PipeWire type, no clock read. Frames at the graph's rate are its one unit; PipeWire's triples
// are converted at the port, in ear_pipewire's latency_spa.hpp.
#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ear/core/sample_rate.hpp"
namespace ear {
class Ports;

/// One pair of a pass-through: each port of `from` (an input bank, routable or inside) copied to the port of `to` (an
/// output bank) at the same member index. The banks have as many ports as each other. Also a path latency crosses a
/// block by (LatencyPlan).
struct BankPair {
	std::string from;
	std::string to;
	friend bool operator==(const BankPair&, const BankPair&) = default;
};

/// A latency in frames at the graph frequency: the least and the most over the paths it stands for. Adds only in
/// series (one path after another); side by side, paths combine only through Paths.
struct Latency {
	std::uint32_t minFrames = 0;
	std::uint32_t maxFrames = 0;
	friend bool operator==(const Latency&, const Latency&) = default;
	/// Series: least + least, most + most (saturating, never wrapping).
	friend constexpr Latency operator+(Latency a, Latency b) noexcept {
		const auto sum = [](std::uint32_t x, std::uint32_t y) {
			return x > std::numeric_limits<std::uint32_t>::max() - y ? std::numeric_limits<std::uint32_t>::max()
			                                                         : x + y;
		};
		return {sum(a.minFrames, b.minFrames), sum(a.maxFrames, b.maxFrames)};
	}
};

/// A latency that lies on every path through where it is declared (a companion's buffer beyond a bank, a code's delay
/// on a pair or a bank): it adds in series and is never a path of its own, so Paths refuses it at compile time. Made
/// only explicitly from a Latency, and never converts back.
struct Delay {
	Latency latency;
	friend bool operator==(const Delay&, const Delay&) = default;
	friend constexpr Latency operator+(Latency path, Delay delay) noexcept { return path + delay.latency; }
	friend constexpr Latency operator+(Delay delay, Latency path) noexcept { return delay.latency + path; }
	friend constexpr Delay operator+(Delay a, Delay b) noexcept { return Delay{a.latency + b.latency}; }
};

/// Parallel paths: the only way to combine latencies side by side, PipeWire's rule (spa_latency_info_combine): the
/// least of the leasts, the most of the mosts. Empty is no path, which is not zero: a port with no peer adds nothing.
class Paths {
public:
	constexpr Paths() noexcept = default;
	constexpr void add(Latency path) noexcept {
		combined_ = combined_ ? Latency{std::min(combined_->minFrames, path.minFrames),
		                                std::max(combined_->maxFrames, path.maxFrames)}
		                      : path;
	}
	/// None is no path, left out.
	constexpr void add(std::optional<Latency> path) noexcept {
		if (path) add(*path);
	}
	/// A delay is in series with a path, never one of its own (a receiver's buffer beside peers reporting zero would
	/// read a least of zero): add it to what this combines, with +.
	template <std::same_as<Delay> D> void add(D) {
		static_assert(false, "a Delay lies in series: add it to a path with +, never to Paths as a path of its own");
	}
	/// None when no path was added.
	constexpr std::optional<Latency> combined() const noexcept { return combined_; }
	/// What a bank with no path tells: zero (spa_latency_info_combine_finish).
	constexpr Latency orZero() const noexcept { return combined_.value_or(Latency{}); }

private:
	std::optional<Latency> combined_;
};

/// Frames as time at the graph's rate, a duration as frames, and the two dating rules (docs/engine.md#dating). The only
/// code that converts between frames and time: each conversion is the exact quotient of the whole count, truncated
/// toward zero, and never overflows on the way (whole seconds and the rest are converted apart).
class FrameClock {
public:
	/// `rate` > 0: the graph's, fixed at boot.
	explicit constexpr FrameClock(std::uint32_t rate) noexcept : rate_(rate) {}
	/// The graph's rate, fixed at boot.
	explicit constexpr FrameClock(SampleRate rate) noexcept : rate_(rate.hz()) {}
	constexpr std::uint32_t rate() const noexcept { return rate_; }
	/// How long `frames` frames last (negative: how long before).
	constexpr std::int64_t ns(std::int64_t frames) const noexcept {
		const std::int64_t rate = rate_;
		return frames / rate * nsPerSecond + frames % rate * nsPerSecond / rate;
	}
	/// How many whole frames `duration` holds: a buffer's, a fade's or a piece's length.
	constexpr std::int64_t frames(std::chrono::nanoseconds duration) const noexcept {
		const std::int64_t ns = duration.count();
		const std::int64_t rate = rate_;
		return ns / nsPerSecond * rate + ns % nsPerSecond * rate / nsPerSecond;
	}
	/// How many frames `duration` holds, rounded up: exact for any period, so a thirtieth of a second
	/// (duration<std::int64_t, std::ratio<1, 30>>{1}) is ceil(rate / 30) frames. The level window's length.
	template <std::integral Rep, class Period>
	constexpr std::int64_t framesCeil(std::chrono::duration<Rep, Period> duration) const noexcept {
		const std::int64_t count = duration.count();
		const std::int64_t rate = rate_;
		const std::int64_t rest = count % Period::den * Period::num * rate;
		const std::int64_t up = rest % Period::den > 0 ? 1 : 0;
		return count / Period::den * Period::num * rate + rest / Period::den + up;
	}
	/// How many frames `duration` holds, rounded to the nearest (half away from zero), exactly: a peer's latency
	/// reported in ns (ear_pipewire's latency_spa.hpp).
	constexpr std::int64_t framesNearest(std::chrono::nanoseconds duration) const noexcept {
		const std::int64_t ns = duration.count();
		const std::int64_t rate = rate_;
		const std::int64_t rest = ns % nsPerSecond * rate;
		const std::int64_t left = rest % nsPerSecond;
		const std::int64_t away = 2 * left >= nsPerSecond ? 1 : (2 * left <= -nsPerSecond ? -1 : 0);
		return ns / nsPerSecond * rate + rest / nsPerSecond + away;
	}
	/// When frame `offset` of a cycle that started at `startRawNs` is heard: once what lies `downstream` of it has
	/// played (its most).
	constexpr std::int64_t heardRawNs(std::int64_t startRawNs, std::uint32_t offset,
	                                  Latency downstream) const noexcept {
		return startRawNs + ns(std::int64_t{offset} + downstream.maxFrames);
	}
	/// When frame `offset` of that cycle (negative: a frame before the cycle's first) entered the graph: what lies
	/// `upstream` of it before it (its most).
	constexpr std::int64_t enteredRawNs(std::int64_t startRawNs, std::int64_t offset, Latency upstream) const noexcept {
		return startRawNs + ns(offset - std::int64_t{upstream.maxFrames});
	}

private:
	static constexpr std::int64_t nsPerSecond = 1'000'000'000;
	std::uint32_t rate_;
};

/// How one configuration's latency crosses its block, by role (docs/engine.md#latency-per-bank-pair): NodeHost makes it
/// with each drive (PortBlock::carryLatency), and the block and the type harness hand it to workOut, its only reader.
/// Each delay lies in series.
struct LatencyPlan {
	std::vector<BankPair> pairs{};                       // the paths the block carries, input bank to output bank
	std::vector<std::pair<BankPair, Delay>> onPair{};    // a code's delay on a pair's path: counts both ways on it
	std::vector<std::pair<std::string, Delay>> onBank{}; // declared on one bank: what that bank alone adds
	std::vector<std::pair<std::string, Delay>> beyond{}; // a companion's, in series beyond a bank's peers
};
/// What the block tells and publishes, worked out afresh from the plan and what the peers report.
struct LatencyFacts {
	/// Per port: inputs then insideInputs (playback, told upstream), then outputs then insideOutputs (capture, told
	/// downstream), each in Ports' order.
	std::vector<Latency> told;
	std::vector<Latency> downstream; // per output port (outputs then insideOutputs): what lies beyond its bank
};
/// The rules, once. `inputPeers` holds one entry per input port (inputs then insideInputs) and `outputPeers` one per
/// output port, none where no peer reported. Beyond a bank: its peers as Paths (none: zero), plus its `beyond` delay.
/// An output bank tells the paths from its paired inputs (each: beyond the input plus the pair's delay) as Paths, none
/// (no pair) zero, plus its `onBank`; an input bank likewise over the pairs it feeds. Each output port's downstream is
/// what lies beyond its bank. Throws std::logic_error for a plan that does not fit `ports` (a bank it lacks, a pair
/// backwards or not in `pairs`, an entry twice) or peers of the wrong count: a caller's bug. The type harness, whose
/// block has no peers, passes none for every port.
LatencyFacts workOut(const Ports& ports, const LatencyPlan& plan, std::span<const std::optional<Latency>> inputPeers,
                     std::span<const std::optional<Latency>> outputPeers);
} // namespace ear
