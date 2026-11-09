// The name a listening node goes by on the network (docs/node-types.md#sendspin). Its records and the socket are the
// system tests', against a real browser.
#include <string>

#include <arpa/inet.h>

#include "advertiser.hpp"
#include "catch2/catch.hpp"

using ear::sendspin::advertisedInstance;
using ear::sendspin::onLink;

namespace {

std::uint32_t address(const char* text) {
	in_addr parsed{};
	REQUIRE(inet_pton(AF_INET, text, &parsed) == 1);
	return parsed.s_addr;
}

} // namespace

TEST_CASE("An instance names the node and its client id, within one DNS label", "[sendspin][advertiser]") {
	CHECK(advertisedInstance("ma", "abcdefghijklmnop") == "ear-ma-abcdefgh");
	const std::string longest = advertisedInstance(std::string(64, 'n'), std::string(43, 'c'));
	CHECK(longest == "ear-" + std::string(50, 'n') + "-" + std::string(8, 'c'));
	CHECK(longest.size() == 63);
}

TEST_CASE("A query is answered only from this link: the local subnet, or this host itself", "[sendspin][advertiser]") {
	const std::uint32_t local = address("192.168.1.10");
	const std::uint32_t mask = address("255.255.255.0");
	CHECK(onLink(address("192.168.1.77"), local, mask));
	CHECK(onLink(address("192.168.1.10"), local, mask));
	CHECK(onLink(address("127.0.0.1"), local, mask));
	CHECK(onLink(address("127.200.0.3"), local, mask));
	CHECK_FALSE(onLink(address("192.168.2.77"), local, mask));
	CHECK_FALSE(onLink(address("8.8.8.8"), local, mask));
	// With no netmask known, only the host's own address.
	CHECK(onLink(local, local, 0xFFFFFFFFU));
	CHECK_FALSE(onLink(address("192.168.1.77"), local, 0xFFFFFFFFU));
}
