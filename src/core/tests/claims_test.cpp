// The claim registry (claims.hpp; docs/writing-a-node-type.md#claims) on its own, by table: who holds a key, a
// claimant refused by a live holder, waiting for a closing one in arrival order, a leaked holder's ports abandoned and
// its stores kept, and a release judging each waiter again. NodeHost's rows (node_host_test.cpp, [claims]) prove it
// at readiness step `claims`, with the probe and the leaked record.
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "claims.hpp"
#include "ear/core/companions.hpp"

using namespace ear;
using namespace ear::detail;
using Outcome = ClaimJudged::Outcome;

namespace {

const Claim udp5004{Claim::Kind::udp, "5004"};
const Claim tcp8928{Claim::Kind::tcp, "8928"};
const Claim key{Claim::Kind::store, "sendspin-ma.key"};

ClaimHolder holder(std::uint64_t generation, const char* node) { return {generation, generation * 10, node}; }

} // namespace

TEST_CASE("A claim's key and its text name its kind", "[claims]") {
	CHECK(claimKey(tcp8928) == "tcp:8928");
	CHECK(claimKey(udp5004) == "udp:5004");
	CHECK(claimKey(key) == "store:sendspin-ma.key");
	CHECK(claimKey({Claim::Kind::recording, "take1.wav"}) == "recording:take1.wav");
	CHECK(claimText(tcp8928, "held by node a") == "cannot listen on port 8928: held by node a");
	CHECK(claimText(udp5004, "Address already in use") == "cannot bind UDP port 5004: Address already in use");
	CHECK(claimText(key, "held by node a") == "sendspin-ma.key: held by node a");
	CHECK(claimText({Claim::Kind::recording, "take1.wav"}, "held by node a") ==
	      "cannot open take1.wav: held by node a");
	CHECK(claim::of(claim::Udp(5004)) == udp5004);
	CHECK(claim::of(claim::TcpListen(8928)) == tcp8928);
	CHECK_THROWS_AS(claim::Udp(0), std::logic_error);
	CHECK_THROWS_AS(claim::TcpListen(65536), std::logic_error);
	// An engine companion's claims come from its shape: an RTP receiver's port whatever its address, a card none.
	const RtpReceiverShape group{*RtpAddress::parse("239.0.0.1"), 5004, RtpEncoding::l24, SampleRate::fixed<48000>(),
	                             100};
	CHECK(claimsOf(group) == std::vector<Claim>{udp5004});
	CHECK(claimsOf(SoundCardShape{*CardId::parse("Pro24")}).empty());
}

TEST_CASE("A free claim is held by its first claimant, a port once probed, a live holder refuses every other, its own "
          "configuration granted again",
          "[claims]") {
	ClaimRegistry registry;
	CHECK(registry.claim(holder(1, "a"), udp5004).outcome == Outcome::probe);
	CHECK(registry.claim(holder(1, "a"), key).outcome == Outcome::granted);
	CHECK(registry.claim(holder(1, "a"), udp5004).outcome == Outcome::granted); // its own: one holder
	const ClaimJudged refused = registry.claim(holder(2, "b"), udp5004);
	CHECK(refused.outcome == Outcome::refused);
	CHECK(refused.why == "held by node a");
	CHECK(refused.leaked == 0);
	CHECK(registry.holds(1));
	CHECK_FALSE(registry.holds(2));
	CHECK(registry.release(1).empty()); // nobody waited
	CHECK_FALSE(registry.holds(1));
	CHECK(registry.claim(holder(2, "b"), udp5004).outcome == Outcome::probe);
}

TEST_CASE("A closing holder makes claimants wait, its release judges them again in arrival order, the first held and "
          "the next refused by it, a waiter whose own teardown began leaves the queue",
          "[claims]") {
	ClaimRegistry registry;
	REQUIRE(registry.claim(holder(1, "a"), udp5004).outcome == Outcome::probe);
	registry.going(1);
	const ClaimJudged waits = registry.claim(holder(2, "b"), udp5004);
	CHECK(waits.outcome == Outcome::waits);
	CHECK(waits.why == "a");
	REQUIRE(registry.claim(holder(3, "c"), udp5004).outcome == Outcome::waits);
	REQUIRE(registry.claim(holder(4, "d"), udp5004).outcome == Outcome::waits);
	registry.going(4); // d is torn down before its grant
	const std::vector<ClaimDecided> decided = registry.release(1);
	REQUIRE(decided.size() == 2);
	CHECK(decided[0].waiter.node == "b");
	CHECK(decided[0].claim == udp5004);
	CHECK(decided[0].judged.outcome == Outcome::probe);
	CHECK(decided[1].waiter.node == "c");
	CHECK(decided[1].judged.outcome == Outcome::refused);
	CHECK(decided[1].judged.why == "held by node b");
	CHECK(registry.holds(2));
}

TEST_CASE("A leaked holder's port is abandoned to the next claimant's probe, its store kept and refusing with the "
          "leaked configuration named, both refusals and the probe say which configuration they are owed to",
          "[claims][leak]") {
	ClaimRegistry registry;
	REQUIRE(registry.claim(holder(1, "a"), udp5004).outcome == Outcome::probe);
	REQUIRE(registry.claim(holder(1, "a"), key).outcome == Outcome::granted);
	REQUIRE(registry.claim(holder(1, "a"), tcp8928).outcome == Outcome::probe);
	registry.going(1);
	// One waits for the store and one for the port when it is leaked: each judged then.
	REQUIRE(registry.claim(holder(2, "a"), key).outcome == Outcome::waits);
	REQUIRE(registry.claim(holder(3, "b"), tcp8928).outcome == Outcome::waits);
	const std::vector<ClaimDecided> decided = registry.leaked(1);
	REQUIRE(decided.size() == 2);
	CHECK(decided[0].waiter.generation == 2);
	CHECK(decided[0].judged.outcome == Outcome::refused);
	CHECK(decided[0].judged.why == "held by node a's earlier configuration, which did not close; it starts when that "
	                               "closes");
	CHECK(decided[0].judged.leaked == 1);
	CHECK(decided[1].waiter.generation == 3);
	CHECK(decided[1].judged.outcome == Outcome::probe);
	CHECK(decided[1].judged.leaked == 1);
	// Later claimants: the port abandoned to the first, the store still kept.
	const ClaimJudged port = registry.claim(holder(4, "c"), udp5004);
	CHECK(port.outcome == Outcome::probe);
	CHECK(port.leaked == 1);
	CHECK(registry.claim(holder(5, "d"), udp5004).why == "held by node c");
	CHECK(registry.claim(holder(6, "a"), key).leaked == 1);
	// Its late close releases what it still holds: the store, which the next claimant gets.
	CHECK(registry.holds(1));
	CHECK(registry.release(1).empty());
	CHECK_FALSE(registry.holds(1));
	CHECK(registry.claim(holder(7, "a"), key).outcome == Outcome::granted);
	CHECK(registry.holds(4));
}
