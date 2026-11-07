// The "card" node type: a sound card behind a port block (docs/design/batch2.md section 4.4, ear/nodes/card.hpp;
// docs/design/node-framework.md section 11). Inputs "play.1".."play.N" pass straight through to the inside bank
// "to_card", which the framework joins to the playback card's channels; the capture card's channels arrive on the inside
// bank "from_card" and pass straight through to the outputs "capture.1"... The pass-through is the framework's: the type
// writes no audio code. The card is named only by a CardId, so no user text reaches ALSA.
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "ear/core/settings_table.hpp"
#include "ear/nodes/card.hpp"

namespace ear::nodes {
namespace {

constexpr std::uint32_t maxChannels = 64;
constexpr auto cardRow = setting::required(
	"card",
	setting::text<CardId::parse>("an ALSA card id, 1-15 characters of [A-Za-z0-9_-] (as in /proc/asound/cards)"));
constexpr auto playbackRow = setting::defaulted("playback", setting::number(0, maxChannels, "channels"), 0);
constexpr auto captureRow = setting::defaulted("capture", setting::number(0, maxChannels, "channels"), 0);
constexpr SettingsTable<cardRow, playbackRow, captureRow> table{"card"};

class CardConfig final : public NodeConfig {
public:
	CardConfig(CardId card, std::uint32_t playback, std::uint32_t capture)
		: card_(std::move(card)), playback_(playback), capture_(capture),
		  ports_{{{Direction::input, "play", numbered(playback)},
	              {Direction::output, "capture", numbered(capture)},
	              {Direction::input, "from_card", numbered(capture), PortBank::Reach::inside},
	              {Direction::output, "to_card", numbered(playback), PortBank::Reach::inside}}} {}

	const Ports& ports() const override { return ports_; }

	StartResult start(StartContext& context) const override {
		if (playback_ > 0)
			context.acquire(SoundCardRequest{.card = card_, .direction = CardDirection::playback, .bank = "to_card"});
		if (capture_ > 0)
			context.acquire(SoundCardRequest{.card = card_, .direction = CardDirection::capture, .bank = "from_card"});
		return Started{passThrough({{"play", "to_card"}, {"from_card", "capture"}})};
	}

private:
	CardId card_;
	std::uint32_t playback_;
	std::uint32_t capture_;
	Ports ports_;
};

class CardType final : public NodeTypeOf<table> {
	MadeConfig make(const NodeName&, const Checked& settings) const override {
		const std::uint32_t playback = settings.get<playbackRow>();
		const std::uint32_t capture = settings.get<captureRow>();
		if (playback == 0 && capture == 0) {
			return std::unexpected(std::string("playback or capture: at least one must be above 0"));
		}
		return std::make_unique<const CardConfig>(settings.get<cardRow>(), playback, capture);
	}
};

} // namespace

std::unique_ptr<const NodeType> makeCardNodeType(const NodeFolders&) { return std::make_unique<const CardType>(); }

} // namespace ear::nodes
