// The "gain" node type's spec and class (docs/node-types.md#gain; docs/writing-a-node-type.md#the-class): a gain per
// channel, every adjustment live but the channel count, which reshapes the node in place. Private to the type, shared by
// gain.cpp and its tests, which run it through ear::testing::TypeHarness<spec, Gain> by the same handles. In the type's
// own named namespace, never an unnamed one (type_spec.hpp says why).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ear/core/lifecycle.hpp"

namespace ear::nodes::gain {

inline constexpr auto channelsRow =
	setting::defaulted("channels", setting::number(1, 64, "channels"), 2u).shapesPorts().inPlace();
inline constexpr auto gainRow =
	setting::defaulted(
		"gainDb",
		setting::decimal(-90.0, 24.0, "dB", {.decimals = 1, .taper = setting::Taper::db, .minIsSilence = true}), 0.0)
		.live(setting::ramp(20));
inline constexpr auto muteRow = setting::defaulted("mute", setting::boolean(), false).live(setting::ramp(10));
inline constexpr auto trimRow =
	setting::indexed("trimDb", channelsRow, setting::decimal(-24.0, 24.0, "dB", {.decimals = 1}), 0.0)
		.live(setting::ramp(20));
inline constexpr auto invertRow =
	setting::indexed("invert", channelsRow, setting::boolean(), false).live(setting::ramp(10));

inline constexpr auto in = bank::input<"in">;
inline constexpr auto out = bank::output<"out">;

inline constexpr auto clipping = status::flag<"clipping">(writer::rt).help("an output went above full scale");
inline constexpr auto clipped = event::of<"clipped">(EventSeverity::warn, "channel {channel} clipped at {peakDb} dB",
                                                     arg::number<"channel">(1, 64), arg::decimal<"peakDb">());
inline constexpr auto resetClip = action::trigger<"reset-clip">("clear the clipping flag");

inline constexpr TypeSpec spec{.settings = SettingsTable<channelsRow, gainRow, muteRow, trimRow, invertRow>{"gain"},
                               .banks = Banks<in, out>{},
                               .status = StatusTable<clipping>{},
                               .events = EventTable<clipped>{},
                               .actions = ActionTable<resetClip>{}};
using Checked = CheckedOf<spec>;

/// 10^(dB / 20), as 2^(dB × log2(10) / 20).
inline float dbToFactor(float db) noexcept { return std::exp2(db * 0.16609640474F); }
/// gainDb's minimum is silence (its row's minIsSilence), whatever the trim adds: exact zeros, never 10^(-90/20).
inline float gainToFactor(float db) noexcept { return db <= -90.0F ? 0.0F : dbToFactor(db); }

struct GainCode {
	/// An output went above full scale since the code was made or reset-clip was last invoked.
	bool latched = false;

	void process(Io<spec> io, const GraphCycle&) noexcept {
		if (io.triggered<resetClip>()) latched = false;
		const Smoothed gain = io.param<gainRow>();
		const Smoothed mute = io.param<muteRow>();
		const auto ins = io[in];
		const auto outs = io[out];
		for (std::size_t c = 0; c < outs.size(); ++c) {
			const Smoothed trim = io.param<trimRow>(c);
			const Smoothed invert = io.param<invertRow>(c);
			// A boolean ramps as a weight from 0 to 1 (docs/writing-a-node-type.md#live-rows): mute fades to 0, invert
			// swings the factor from 1 through 0 to -1.
			const auto factorAt = [&](std::uint32_t i) {
				return gainToFactor(gain[i]) * dbToFactor(trim[i]) * (1.0F - mute[i]) * (1.0F - 2.0F * invert[i]);
			};
			const std::span<const float> source = ins[c];
			const std::span<float> sink = outs[c];
			if (gain.steady() && trim.steady() && mute.steady() && invert.steady()) {
				const float factor = factorAt(0);
				for (std::uint32_t i = 0; i < io.frames(); ++i) sink[i] = source[i] * factor;
			} else {
				for (std::uint32_t i = 0; i < io.frames(); ++i) sink[i] = source[i] * factorAt(i);
			}
			float peak = 0.0F;
			for (std::uint32_t i = 0; i < io.frames(); ++i) peak = std::max(peak, std::abs(sink[i]));
			if (peak > 1.0F && !latched) {
				latched = true;
				io.raise<clipped>(static_cast<std::int64_t>(c + 1), 20.0 * std::log10(static_cast<double>(peak)));
			}
		}
		io.set<clipping>(latched);
	}
};

struct Gain {
	static Members<spec> ports(const Checked& s) {
		return {in(members(s.get<channelsRow>())), out(members(s.get<channelsRow>()))};
	}
	Configured configure(CreateOf<spec>& cx, const Checked&) { return cx.applied(GainCode{}); }
	/// Called only for a change of channels, its one row that is not live: a code for the new ports, its latch clear.
	/// The live rows' slots stay with the configuration, so every ramp in flight goes on across the swap.
	Changed configure(ChangeOf<spec>& cx, const Checked&, const Checked&) { return cx.applied(GainCode{}); }
};

} // namespace ear::nodes::gain
