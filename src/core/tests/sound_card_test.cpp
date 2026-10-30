// The card id's closed grammar (docs/design/batch2.md section 4.1, sound_card.hpp; C1 criterion
// 15): only ALSA card ids parse, so no user text reaches alsa-lib's device-name parser.
#include <string>

#include "catch2/catch.hpp"
#include "ear/core/sound_card.hpp"

using namespace ear;

TEST_CASE("A card id is 1-15 characters of letters, digits, '_' and '-'", "[card_id]") {
	for (const std::string text : {"Pro24", "M24_ao-2", "a", "abcdefghijklmno"}) {
		INFO(text);
		const auto id = CardId::parse(text);
		REQUIRE(id.has_value());
		CHECK(id->text() == text);
	}
	CHECK(CardId::parse("Pro24") == CardId::parse("Pro24"));
}

TEST_CASE("A card id refuses anything else, so no ALSA device string can be named", "[card_id]") {
	for (const std::string text : {"", "abcdefghijklmnop", "hw:0", "file:'|touch x'", "a,b", "a b", "a=b", "a.b",
	                               "a|b", "a/b", "Pro24\n"}) {
		INFO(text);
		CHECK_FALSE(CardId::parse(text).has_value());
	}
}
