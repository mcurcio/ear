// The RTP address's closed grammar (docs/design/rtp.md section 3; R1a criteria 1 and 2): "0.0.0.0"
// or an IPv4 multicast group as a plain dotted quad, nothing else.
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/rtp_receiver.hpp"

using ear::RtpAddress;

TEST_CASE("RtpAddress accepts any-address and multicast groups, as given", "[rtp_address]") {
	const auto any = RtpAddress::parse("0.0.0.0");
	REQUIRE(any.has_value());
	CHECK(any->text() == "0.0.0.0");
	CHECK_FALSE(any->multicast());

	for (const std::string text : {"224.0.0.1", "239.69.0.1", "239.255.255.255", "224.0.0.0"}) {
		INFO(text);
		const auto group = RtpAddress::parse(text);
		REQUIRE(group.has_value());
		CHECK(group->text() == text);
		CHECK(group->multicast());
	}
	CHECK(*RtpAddress::parse("239.69.0.1") == *RtpAddress::parse("239.69.0.1"));
	CHECK_FALSE(*RtpAddress::parse("239.69.0.1") == *RtpAddress::parse("239.69.0.2"));
}

TEST_CASE("RtpAddress refuses unicast addresses, names, IPv6 and anything not a plain dotted quad",
          "[rtp_address]") {
	const std::vector<std::string> refused{
	    "",           "127.0.0.1",   "192.168.1.10", "10.0.0.1",    "223.255.255.255", "240.0.0.1",
	    "255.255.255.255", "fpp.local", "::",       "ff02::1",     "239.1.1",         "239.1.1.1.1",
	    "239.01.0.1", "239.256.0.1", " 239.1.1.1",   "239.1.1.1 ",  "0x7f.0.0.1",      "239.1.1.1/24",
	    "239..1.1",   "239.1.1.",    ".239.1.1.1",   "+239.1.1.1",  "239.1.1.1000",    "00.0.0.0",
	};
	for (const auto& text : refused) {
		INFO("\"" << text << "\"");
		CHECK_FALSE(RtpAddress::parse(text).has_value());
	}
}
