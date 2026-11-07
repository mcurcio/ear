// The "card" node type: a sound card behind a port block (docs/design/batch2.md section 4.4,
// ear/nodes/card.hpp). Inputs "play.1".."play.N" pass straight through to the inside outputs
// "to_card_1".., which the framework joins to the playback card's channels; the capture card's
// channels arrive on the inside inputs "from_card_1".. and pass straight through to the outputs
// "capture.1"... The card is named only by a CardId, so no user text reaches ALSA.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
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

/// Copies play.i to to_card_i and from_card_j to capture.j. With the framework's order
/// (inputs then insideInputs, outputs then insideOutputs): inputs are play then from_card,
/// outputs are capture then to_card.
class PassThrough final : public AudioCode {
public:
	PassThrough(std::uint32_t playback, std::uint32_t capture) noexcept : playback_(playback), capture_(capture) {}

	void prepare(const AudioFormat&) override {}

	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle&) noexcept override {
		for (std::uint32_t i = 0; i < playback_; ++i) {
			std::ranges::copy(inputs[i].first(frames), outputs[capture_ + i].begin());
		}
		for (std::uint32_t j = 0; j < capture_; ++j) {
			std::ranges::copy(inputs[playback_ + j].first(frames), outputs[j].begin());
		}
	}

	void release() noexcept override {}

private:
	std::uint32_t playback_;
	std::uint32_t capture_;
};

class CardInsides final : public NodeInsides {
public:
	CardInsides(std::uint32_t playback, std::uint32_t capture) noexcept : code_(playback, capture) {}
	AudioCode& audioCode() override { return code_; }

private:
	PassThrough code_;
};

class CardConfig final : public NodeConfig {
public:
	CardConfig(CardId card, std::uint32_t playback, std::uint32_t capture)
		: card_(std::move(card)), playback_(playback), capture_(capture),
		  ports_{{{Direction::input, "play", numbered(playback)}, {Direction::output, "capture", numbered(capture)}},
	             numbered(capture, "from_card_"),
	             numbered(playback, "to_card_")} {}

	const Ports& ports() const override { return ports_; }

	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& grant) const override {
		if (playback_ > 0) {
			grant.soundCard(
				{.card = card_, .direction = CardDirection::playback, .blockPorts = ports_.insideOutputs()});
		}
		if (capture_ > 0) {
			grant.soundCard({.card = card_, .direction = CardDirection::capture, .blockPorts = ports_.insideInputs()});
		}
		return std::make_unique<CardInsides>(playback_, capture_);
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

std::unique_ptr<const NodeType> makeCardNodeType() { return std::make_unique<const CardType>(); }

} // namespace ear::nodes
