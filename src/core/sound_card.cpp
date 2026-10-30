// The card id, the one way anything names a sound card (docs/design/batch2.md section 4.1,
// sound_card.hpp). Its grammar is closed, so no user text reaches alsa-lib's device-name parser.
#include "ear/core/sound_card.hpp"

#include <algorithm>
#include <utility>

namespace ear {

std::optional<CardId> CardId::parse(std::string_view text) {
	if (text.empty() || text.size() > 15) return std::nullopt;
	const bool closed = std::all_of(text.begin(), text.end(), [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
	});
	if (!closed) return std::nullopt;
	return CardId(std::string(text));
}

const std::string& CardId::text() const noexcept { return text_; }

CardId::CardId(std::string text) : text_(std::move(text)) {}

} // namespace ear
