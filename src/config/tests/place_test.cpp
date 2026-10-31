// placeIn: knobly's places (line and byte column from 0) to the wire's (line from 1, column from 1
// in UTF-16 code units) (docs/design/knobly-config.md, "Wire additions"; S1 criterion 7).
#include "catch2/catch.hpp"
#include "place.hpp"

using ear::config::placeIn;
using ear::control::TextPlace;

TEST_CASE("placeIn counts ASCII lines and columns from 1", "[place]") {
	CHECK(placeIn("abc\ndef", 0, 0) == TextPlace{1, 1});
	CHECK(placeIn("abc\ndef", 1, 2) == TextPlace{2, 3});
}

TEST_CASE("placeIn counts a character before the column once, in UTF-16 code units", "[place]") {
	CHECK(placeIn("\xC3\xA9=x", 0, 3) == TextPlace{1, 3});           // e acute: 2 bytes, 1 unit
	CHECK(placeIn("\xF0\x9F\x98\x80z", 0, 4) == TextPlace{1, 3});    // an emoji: 4 bytes, 2 units
	CHECK(placeIn("a\n\xE2\x82\xAC\xE2\x82\xAC!", 1, 6) == TextPlace{2, 3});   // two euro signs: 3 bytes each
}

TEST_CASE("placeIn counts a character a byte column falls inside", "[place]") {
	CHECK(placeIn("\xF0\x9F\x98\x80z", 0, 2) == TextPlace{1, 3});
	CHECK(placeIn("\xC3\xA9=x", 0, 1) == TextPlace{1, 2});
}

TEST_CASE("placeIn clamps a line past the end and a column past a line's end", "[place]") {
	CHECK(placeIn("a\nbc", 5, 1) == TextPlace{2, 2});
	CHECK(placeIn("a\nbc", 0, 99) == TextPlace{1, 2});
	CHECK(placeIn("a\nbc", 1, 99) == TextPlace{2, 3});
	CHECK(placeIn("a\n", 7, 0) == TextPlace{2, 1});   // a text ending in a line feed has an empty last line
	CHECK(placeIn("", 3, 3) == TextPlace{1, 1});
}
