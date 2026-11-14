// What reaches a status line from a peer (docs/node-types.md#notes). How many notes can is the framework's rate, tested
// with it (src/core/tests/note_gate_test.cpp, node_handle_test.cpp).
#include <string>

#include "catch2/catch.hpp"
#include "notes.hpp"

using namespace ear::sendspin;

TEST_CASE("printable turns every control character into ?", "[sendspin][notes]") {
	CHECK(notes::printable("plain name") == "plain name");
	CHECK(notes::printable("a\nb\rc\td\x1b[2J") == "a?b?c?d?[2J");
	CHECK(notes::printable(std::string("nul\0end", 7)) == "nul?end");
	CHECK(notes::printable("del\x7f") == "del?");
	CHECK(notes::printable("c1\xc2\x9b"
	                       "2J") == "c1?2J"); // U+009B, CSI on a terminal that honours C1
	CHECK(notes::printable("nel\xc2\x85"
	                       "x") == "nel?x"); // U+0085
	CHECK(notes::printable("ls\xe2\x80\xa8ps\xe2\x80\xa9") == "ls?ps?");
	CHECK(notes::printable("Küche \xe2\x82\xac") == "Küche \xe2\x82\xac"); // ordinary characters stay
}

TEST_CASE("printable turns bytes that are no UTF-8 into ?", "[sendspin][notes]") {
	CHECK(notes::printable("bad\xff") == "bad?");
	CHECK(notes::printable("\xc0\x80") == "??");       // overlong
	CHECK(notes::printable("\xed\xa0\x80") == "???");  // a surrogate
	CHECK(notes::printable("cut\xe2\x82") == "cut??"); // a character with no end
}

TEST_CASE("printable reads at most 64 bytes, and never stops inside a character", "[sendspin][notes]") {
	CHECK(notes::printable(std::string(64, 'a')) == std::string(64, 'a'));
	CHECK(notes::printable(std::string(65, 'a')) == std::string(64, 'a'));
	// Byte 64 is the second of a two-byte character: the cut goes back to its start.
	CHECK(notes::printable(std::string(63, 'a') + "\xc3\xa9zz") == std::string(63, 'a'));
	CHECK(notes::printable(std::string(62, 'a') + "\xc3\xa9zz") == std::string(62, 'a') + "\xc3\xa9");
}

TEST_CASE("A server's name is quoted on the line, so it cannot pass for the word after it", "[sendspin][notes]") {
	CHECK(notes::connected("MA", true) == "connected \"MA\" paired");
	CHECK(notes::connected("x paired", false) == "connected \"x paired\" unpaired");
	CHECK(notes::connected("say \"no\"", true) == "connected \"say ?no?\" paired");
	CHECK(notes::connected("line\nnode ma says paired x", false) ==
	      "connected \"line?node ma says paired x\" unpaired");
}
