// The card id, the one way anything names a sound card (docs/design/batch2.md section 4.1,
// sound_card.hpp). Slice s0 stub: slice C1 builds parse() and its grammar.
#include "ear/core/sound_card.hpp"

#include <stdexcept>
#include <utility>

namespace ear {

std::optional<CardId> CardId::parse(std::string_view) {
	throw std::logic_error("CardId::parse: not built yet (slice C1)");
}

const std::string& CardId::text() const noexcept { return text_; }

CardId::CardId(std::string text) : text_(std::move(text)) {}

} // namespace ear
