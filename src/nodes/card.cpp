// The "card" node type: a sound card behind a port block (docs/design/batch2.md section 4.4,
// builtin.hpp). Inputs "play.1".."play.N" pass straight through to the inside outputs
// "to_card_1".., which the framework joins to the playback card's channels; the capture card's
// channels arrive on the inside inputs "from_card_1".. and pass straight through to the outputs
// "capture.1"... The card is named only by a CardId, so no user text reaches ALSA.
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ear/nodes/builtin.hpp"

namespace ear::nodes {
namespace {

constexpr std::string_view typeName = "card";
constexpr std::string_view cardKey = "card";
constexpr std::string_view playbackKey = "playback";
constexpr std::string_view captureKey = "capture";
/// How each key is written in the graph file: the id is text, the channel counts whole numbers.
constexpr std::array<SettingDeclaration, 3> settingDeclarations{
	{{cardKey, SettingKind::text}, {playbackKey, SettingKind::wholeNumber}, {captureKey, SettingKind::wholeNumber}}};
constexpr std::uint32_t maxChannels = 64;

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

class CardType final : public NodeType {
public:
	std::string_view name() const override { return typeName; }

	std::span<const SettingDeclaration> settingKinds() const override { return settingDeclarations; }

	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings>
	parse(const NodeName& node, const NodeSettings& settings) const override {
		if (!settings.is_object()) {
			return std::unexpected(InvalidSettings{node, "settings: must be an object with \"card\""});
		}
		for (const auto& [key, value] : settings.items()) {
			if (key != cardKey && key != playbackKey && key != captureKey) {
				return std::unexpected(InvalidSettings{node, "unknown key " + key});
			}
		}
		const auto card = settings.find(cardKey);
		if (card == settings.end()) {
			return std::unexpected(InvalidSettings{node, "card: missing"});
		}
		std::optional<CardId> id =
			card->is_string() ? CardId::parse(card->get_ref<const std::string&>()) : std::nullopt;
		if (!id) {
			return std::unexpected(InvalidSettings{node, "card: must be an ALSA card id, 1-15 characters of "
			                                             "[A-Za-z0-9_-] (as in /proc/asound/cards)"});
		}
		const auto playback = channels(settings, playbackKey);
		const auto capture = channels(settings, captureKey);
		if (!playback || !capture) {
			return std::unexpected(InvalidSettings{node, std::string(playback ? captureKey : playbackKey) +
			                                                 ": must be a whole number of channels, 0-64"});
		}
		if (*playback == 0 && *capture == 0) {
			return std::unexpected(InvalidSettings{node, "playback or capture: at least one must be above 0"});
		}
		return std::make_unique<const CardConfig>(std::move(*id), *playback, *capture);
	}

private:
	/// The key's channel count, 0 when absent; nullopt when not an integer in 0-64.
	static std::optional<std::uint32_t> channels(const NodeSettings& settings, std::string_view key) {
		const auto value = settings.find(key);
		if (value == settings.end()) {
			return 0U;
		}
		if (!value->is_number_integer()) {
			return std::nullopt;
		}
		const auto count = value->get<std::int64_t>();
		if (count < 0 || count > maxChannels) {
			return std::nullopt;
		}
		return static_cast<std::uint32_t>(count);
	}
};

} // namespace

std::unique_ptr<const NodeType> makeCardNodeType() { return std::make_unique<const CardType>(); }

} // namespace ear::nodes
