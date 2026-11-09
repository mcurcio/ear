#pragma once
// ear's one latency model (docs/engine.md#latency-per-bank-pair): the only code that combines latency or turns frames
// into time. Pure: no PipeWire type, no clock read. Frames at the graph's rate are its one unit; PipeWire's triples
// are converted at the port, in ear_pipewire's latency_spa.hpp.
#include <algorithm>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace ear {
class Ports;

/// One pair of a pass-through: each port of `from` (an input bank, routable or inside) copied to the port of `to` (an
/// output bank) at the same member index. The banks have as many ports as each other. Also a path latency crosses a
/// block by (latency_model::LatencyPlan).
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

/// Frames as time at the graph's rate, and the two dating rules (docs/engine.md#sinks, docs/engine.md#sources). The
/// only code that turns frames into ns: one division of the whole frame count, truncated toward zero.
class FrameClock {
public:
	/// `rate` > 0: the graph's, fixed at boot.
	explicit constexpr FrameClock(std::uint32_t rate) noexcept : rate_(rate) {}
	constexpr std::uint32_t rate() const noexcept { return rate_; }
	/// How long `frames` frames last (negative: how long before).
	constexpr std::int64_t ns(std::int64_t frames) const noexcept { return frames * 1'000'000'000 / rate_; }
	/// When frame `offset` of a cycle that started at `startRawNs` is heard: once what lies `downstream` of it has
	/// played (its most).
	constexpr std::int64_t heardRawNs(std::int64_t startRawNs, std::uint32_t offset,
	                                  Latency downstream) const noexcept {
		return startRawNs + ns(std::int64_t{offset} + downstream.maxFrames);
	}
	/// When frame `offset` of that cycle entered the graph: what lies `upstream` of it before it (its most).
	constexpr std::int64_t enteredRawNs(std::int64_t startRawNs, std::uint32_t offset,
	                                    Latency upstream) const noexcept {
		return startRawNs + ns(std::int64_t{offset} - std::int64_t{upstream.maxFrames});
	}

private:
	std::uint32_t rate_;
};

/// The plan and the rules by role, beside node_host.hpp's ear::LatencyPlan until step R2 of
/// docs/design/root-classes.md moves the block and the harness onto them (and this namespace's names into ear).
namespace latency_model {
/// What a configuration declares, by role (docs/engine.md#latency-per-bank-pair). Each delay lies in series.
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
/// backwards or not in `pairs`, an entry twice) or peers of the wrong count: a caller's bug.
LatencyFacts workOut(const Ports& ports, const LatencyPlan& plan, std::span<const std::optional<Latency>> inputPeers,
                     std::span<const std::optional<Latency>> outputPeers);
} // namespace latency_model
} // namespace ear
