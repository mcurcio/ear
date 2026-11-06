// The name a listening node goes by on the network (docs/design/sendspin.md section 4). Its records and the socket are the
// system tests', against a real browser.
#include <string>

#include "advertiser.hpp"
#include "catch2/catch.hpp"

using ear::sendspin::advertisedInstance;

TEST_CASE("An instance names the node and its client id, within one DNS label", "[sendspin][advertiser]") {
	CHECK(advertisedInstance("ma", "abcdefghijklmnop") == "ear-ma-abcdefgh");
	const std::string longest = advertisedInstance(std::string(64, 'n'), std::string(43, 'c'));
	CHECK(longest == "ear-" + std::string(50, 'n') + "-" + std::string(8, 'c'));
	CHECK(longest.size() == 63);
}
