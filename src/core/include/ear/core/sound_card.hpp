#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "ear/core/graph_spec.hpp"
namespace ear {
/// An ALSA card id, the name in /proc/asound/cards (e.g. "Pro24"): 1-15 characters of
/// [A-Za-z0-9_-]. The only way anything names a sound card, so no user text ever reaches
/// alsa-lib's device-name parser, whose plugins open files and run commands
/// (the `file` plugin's "|command" form).
class CardId {
public:
	static std::optional<CardId> parse(std::string_view text); // nullopt outside the grammar
	const std::string& text() const noexcept;
	friend bool operator==(const CardId&, const CardId&) = default;

private:
	explicit CardId(std::string text);
	std::string text_;
};
} // namespace ear
