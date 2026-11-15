// shairport-sync's report feed read (ReportParser) and its listing read (listingOf): a real captured feed split at
// every boundary, items too long or malformed dropped and counted, and every way a listing is refused.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "captured_feed.hpp"
#include "catch2/catch.hpp"
#include "reports.hpp"

namespace airplay_test {
namespace airplay = ear::nodes::airplay;

std::vector<airplay::Report> whole(std::string_view text) {
	airplay::ReportParser parser;
	return parser.feed(text);
}

std::uint32_t code(std::string_view four) {
	std::uint32_t value = 0;
	for (const char c : four) value = (value << 8U) | static_cast<unsigned char>(c);
	return value;
}

std::string item(std::string_view four, std::string_view data, std::string_view base64) {
	std::string text = "<item><type>73736e63</type><code>";
	constexpr std::string_view hex = "0123456789abcdef";
	for (const char c : four) {
		text += hex[static_cast<unsigned char>(c) >> 4U];
		text += hex[static_cast<unsigned char>(c) & 0xFU];
	}
	text += "</code><length>" + std::to_string(data.size()) + "</length>";
	if (!data.empty()) text += "\n<data encoding=\"base64\">\n" + std::string(base64) + "</data>";
	return text + "</item>\n";
}

} // namespace airplay_test

using namespace airplay_test;

TEST_CASE("ReportParser reads the captured feed's 27 items, decoding each one's base64 data", "[airplay][reports]") {
	const auto reports = whole(capturedFeed);
	REQUIRE(reports.size() == 27);
	CHECK(reports[0] == airplay::Report{airplay::ssnc, code("svna"), "Kitchen"});
	CHECK(reports[1].code == airplay::svrg);
	CHECK(reports[1].data.starts_with("service=_raop._tcp\nname=D6065625AF29@Kitchen\nport=5100\ntxt=sf=0x4\n"));
	CHECK(reports[16] == airplay::Report{airplay::ssnc, airplay::pbeg, ""});
	CHECK(reports[24] == airplay::Report{airplay::ssnc, airplay::pend, ""});
	CHECK(reports[25].code == airplay::svrg);
}

TEST_CASE("ReportParser gives the same items whichever boundary splits the captured feed into two chunks",
          "[airplay][reports]") {
	const auto expected = whole(capturedFeed);
	for (std::size_t cut = 0; cut <= capturedFeed.size(); ++cut) {
		INFO("cut at byte " << cut);
		airplay::ReportParser parser;
		auto reports = parser.feed(capturedFeed.substr(0, cut));
		for (auto& report : parser.feed(capturedFeed.substr(cut))) reports.push_back(std::move(report));
		REQUIRE(reports == expected);
		REQUIRE(parser.dropped() == 0);
	}
}

TEST_CASE("ReportParser gives the same items fed one byte at a time", "[airplay][reports]") {
	airplay::ReportParser parser;
	std::vector<airplay::Report> reports;
	for (const char c : capturedFeed)
		for (auto& report : parser.feed(std::string_view(&c, 1))) reports.push_back(std::move(report));
	CHECK(reports == whole(capturedFeed));
	CHECK(parser.dropped() == 0);
}

TEST_CASE("ReportParser drops an item past 64 KiB up to the next item and counts it, keeping what follows",
          "[airplay][reports]") {
	const std::string art(airplay::maxItemBytes, 'A'); // base64 of cover art, too long to keep
	const std::string text = item("PICT", std::string(art.size() / 4 * 3, '\0'), art) + item("pbeg", "", "");
	SECTION("whole") {
		airplay::ReportParser parser;
		CHECK(parser.feed(text) == std::vector<airplay::Report>{{airplay::ssnc, airplay::pbeg, ""}});
		CHECK(parser.dropped() == 1);
	}
	SECTION("in chunks of 1000 bytes, never holding more than the limit") {
		airplay::ReportParser parser;
		std::vector<airplay::Report> reports;
		for (std::size_t at = 0; at < text.size(); at += 1000)
			for (auto& report : parser.feed(std::string_view(text).substr(at, 1000))) reports.push_back(report);
		CHECK(reports == std::vector<airplay::Report>{{airplay::ssnc, airplay::pbeg, ""}});
		CHECK(parser.dropped() == 1);
	}
}

TEST_CASE("ReportParser skips garbage and malformed items to the next item, counting each", "[airplay][reports]") {
	airplay::ReportParser parser;
	const std::string text = "junk" + item("pbeg", "", "") + "<item><type>zz</type></item>\n" +
	                         item("svna", "Kitchen", "S2l0Y2hlbg=") + item("pend", "", "") +
	                         item("svna", "Kitchen", "S2l0Y2hlbg==");
	CHECK(parser.feed(text) == std::vector<airplay::Report>{{airplay::ssnc, airplay::pbeg, ""},
	                                                        {airplay::ssnc, airplay::pend, ""},
	                                                        {airplay::ssnc, code("svna"), "Kitchen"}});
	CHECK(parser.dropped() == 3); // the junk, the bad type, the bad base64
}

TEST_CASE("listingOf reads service, name, port and every txt line onto the node's host", "[airplay][reports]") {
	const auto listing = airplay::listingOf(
		"service=_raop._tcp\nname=AB12@Kitchen\nport=5100\ntxt=tp=UDP\ntxt=sr=44100\n", "ear-kitchen");
	REQUIRE(listing.has_value());
	CHECK(*listing == ear::mdns::Listing{.service = "_raop._tcp",
	                                     .instance = "AB12@Kitchen",
	                                     .host = "ear-kitchen",
	                                     .port = 5100,
	                                     .txt = {"tp=UDP", "sr=44100"}});
}

TEST_CASE("listingOf refuses a listing missing a line, with a bad port, or one the advertiser refuses",
          "[airplay][reports]") {
	const auto why = [](std::string_view data) {
		const auto listing = airplay::listingOf(data, "ear-kitchen");
		return listing ? std::string("accepted") : listing.error();
	};
	CHECK(why("name=a@K\nport=1\n") == "the listing has no service line");
	CHECK(why("service=_raop._tcp\nport=1\n") == "the listing has no name line");
	CHECK(why("service=_raop._tcp\nname=a@K\n") == "the listing has no port line");
	CHECK(why("service=_raop._tcp\nname=a@K\nport=0\n") == "the listing's port 0 is not 1 to 65535");
	CHECK(why("service=_raop._tcp\nname=a@K\nport=65536\n") == "the listing's port 65536 is not 1 to 65535");
	CHECK(why("service=_raop._tcp\nname=a@K\nport=x\n") == "the listing's port x is not 1 to 65535");
	CHECK(why("service=raop\nname=a@K\nport=1\n") ==
	      *ear::mdns::listingRefused({.service = "raop", .instance = "a@K", .host = "ear-kitchen", .port = 1}));
	CHECK(why("service=_raop._tcp\nname=a@K\nport=1\ntxt=novalue\n") == "a TXT string needs key=value");
}
