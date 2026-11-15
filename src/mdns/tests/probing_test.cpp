// The pure parts of probing (src/mdns/probing.hpp; docs/node-types.md#mdns), with no socket: the names a clash gives,
// the record keys section 8.2 compares, its tiebreak, and the pace probing keeps after many clashes.
#include <chrono>
#include <string>
#include <vector>

#include <arpa/inet.h>

#include "catch2/catch.hpp"
#include "probing.hpp"

namespace probing = ear::mdns::probing;
using probing::RecordKey;
using probing::Tiebreak;

TEST_CASE("A renamed instance is '<asked> (n)', cut on a character's boundary to 63 bytes", "[mdns][probing]") {
	CHECK(probing::instanceNamed("Kitchen", 2) == "Kitchen (2)");
	CHECK(probing::instanceNamed("Kitchen", 13) == "Kitchen (13)");
	// 59 bytes fit before " (2)": a 63-byte name whose bytes 58 and 59 are one character "é" loses that character
	// whole, never half of it.
	const std::string asked = std::string(58, 'a') + "\xC3\xA9" + "bbb";
	REQUIRE(asked.size() == 63);
	CHECK(probing::instanceNamed(asked, 2) == std::string(58, 'a') + " (2)");
	const std::string ascii(63, 'k');
	CHECK(probing::instanceNamed(ascii, 2) == std::string(59, 'k') + " (2)");
	CHECK(probing::instanceNamed(ascii, 2).size() == 63);
}

TEST_CASE("A renamed host is '<asked>-n', cut to 63 bytes", "[mdns][probing]") {
	CHECK(probing::hostNamed("ear-kitchen", 2) == "ear-kitchen-2");
	CHECK(probing::hostNamed(std::string(63, 'h'), 2) == std::string(61, 'h') + "-2");
	CHECK(probing::hostNamed(std::string(63, 'h'), 10) == std::string(60, 'h') + "-10");
}

TEST_CASE("The record keys are the class, the type and the uncompressed rdata the advertiser sends",
          "[mdns][probing]") {
	const RecordKey srv = probing::srvKey(0x1234, "ear-k.local.");
	CHECK(srv.rrclass == 1);
	CHECK(srv.type == 33);
	CHECK(srv.rdata == std::string("\0\0\0\0\x12\x34\x05"
	                               "ear-k\x05local\0",
	                               19));
	CHECK(probing::srvKey(0x1234, "ear-k.local") == srv);

	const RecordKey txt = probing::txtKey({"a=1", "path=/x"});
	CHECK(txt.rrclass == 1);
	CHECK(txt.type == 16);
	CHECK(txt.rdata == std::string("\x03"
	                               "a=1\x07"
	                               "path=/x"));

	in_addr address{};
	REQUIRE(inet_pton(AF_INET, "192.0.2.7", &address) == 1);
	const RecordKey a = probing::addressKey(address.s_addr);
	CHECK(a.rrclass == 1);
	CHECK(a.type == 1);
	CHECK(a.rdata == std::string("\xC0\x00\x02\x07", 4));
}

TEST_CASE("The tiebreak: the later set wins, identical sets are no conflict, and a prefix loses", "[mdns][probing]") {
	const RecordKey low = probing::srvKey(1000, "ear-test-host.local.");
	const RecordKey high = probing::srvKey(2000, "ear-test-host.local.");
	const RecordKey txt = probing::txtKey({"a=1"});
	CHECK(probing::tiebreak({high, txt}, {low, txt}) == Tiebreak::ours);
	CHECK(probing::tiebreak({low, txt}, {high, txt}) == Tiebreak::theirs);
	CHECK(probing::tiebreak({low, txt}, {low, txt}) == Tiebreak::same);
	// The inputs' order does not matter: each set is sorted first.
	CHECK(probing::tiebreak({txt, high}, {low, txt}) == Tiebreak::ours);
	CHECK(probing::tiebreak({txt, low}, {txt, high}) == Tiebreak::theirs);
	CHECK(probing::tiebreak({txt, low}, {low, txt}) == Tiebreak::same);
	// A set that runs out first, every record equal to the other's, loses.
	CHECK(probing::tiebreak({txt}, {txt, low}) == Tiebreak::theirs);
	CHECK(probing::tiebreak({low, txt}, {txt}) == Tiebreak::ours);
	// Bytes compare as unsigned: 0x80 is later than 0x7F.
	CHECK(probing::tiebreak({probing::addressKey(0x80)}, {probing::addressKey(0x7F)}) == Tiebreak::ours);
}

TEST_CASE("After 15 clashes in 10 s each further clash waits 5 s first, and a clash after the window none",
          "[mdns][probing]") {
	using namespace std::chrono_literals;
	probing::ConflictPace pace;
	const auto start = probing::ConflictPace::Clock::time_point{} + 1h;
	for (int i = 0; i < 15; ++i) CHECK(pace.clashed(start + i * 100ms) == 0ms);
	CHECK(pace.clashed(start + 1600ms) == 5000ms);
	CHECK(pace.clashed(start + 1700ms) == 5000ms);
	// Long after: the window holds this clash alone.
	CHECK(pace.clashed(start + 20s) == 0ms);
}
