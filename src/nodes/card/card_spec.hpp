// The "card" node type's spec and class (docs/node-types.md#card; docs/writing-a-node-type.md#the-class): private to
// the type, shared by card.cpp and its tests, which run it through ear::testing::TypeHarness<spec, Card> by the same
// bank handles. In the type's own named namespace, never an unnamed one (type_spec.hpp says why). Inputs
// "play.1".."play.N" pass straight through to the inside bank "to_card", which the framework joins to the playback
// card's channels; the capture card's channels arrive on the inside bank "from_card" and pass straight through to the
// outputs "capture.1"... The pass-through is the framework's: the type writes no audio code. The card is named only by
// a CardId, so no user text reaches ALSA.
#pragma once
#include <cstdint>
#include <expected>
#include <string>

#include "ear/core/lifecycle.hpp"

namespace ear::nodes::card {

inline constexpr std::uint32_t maxChannels = 64;
inline constexpr auto cardRow = setting::required(
	"card",
	setting::text<CardId::parse>("an ALSA card id, 1-15 characters of [A-Za-z0-9_-] (as in /proc/asound/cards)"));
inline constexpr auto playbackRow = setting::defaulted("playback", setting::number(0, maxChannels, "channels"), 0);
inline constexpr auto captureRow = setting::defaulted("capture", setting::number(0, maxChannels, "channels"), 0);
inline constexpr auto play = bank::input<"play">;
inline constexpr auto capture = bank::output<"capture">;
inline constexpr auto fromCard = bank::insideInput<"from_card">;
inline constexpr auto toCard = bank::insideOutput<"to_card">;
inline constexpr TypeSpec spec{.settings = SettingsTable<cardRow, playbackRow, captureRow>{"card"},
                               .banks = Banks<play, capture, fromCard, toCard>{}};
using Checked = CheckedOf<spec>;

struct Card {
	static Members<spec> ports(const Checked& s) {
		return {play(members(s.get<playbackRow>())), capture(members(s.get<captureRow>())),
		        fromCard(members(s.get<captureRow>())), toCard(members(s.get<playbackRow>()))};
	}

	static std::expected<void, std::string> check(const Checked& s) {
		if (s.get<playbackRow>() == 0 && s.get<captureRow>() == 0)
			return std::unexpected(std::string("playback or capture: at least one must be above 0"));
		return {};
	}

	Configured configure(CreateOf<spec>& cx, const Checked& s) {
		if (s.get<playbackRow>() > 0) cx.soundCard(toCard, SoundCardShape{.card = s.get<cardRow>()});
		if (s.get<captureRow>() > 0) cx.soundCard(fromCard, SoundCardShape{.card = s.get<cardRow>()});
		cx.use(passThrough({bankPair(play, toCard), bankPair(fromCard, capture)}));
		return Configured::applied();
	}
};

} // namespace ear::nodes::card
