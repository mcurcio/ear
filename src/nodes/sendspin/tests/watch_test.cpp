// The connection watch, on fake time (docs/node-types.md#failures).
#include <chrono>

#include "catch2/catch.hpp"
#include "connection_watch.hpp"

using ear::sendspin::ConnectionWatch;
using namespace std::chrono_literals;

namespace {

const auto zero = std::chrono::steady_clock::time_point{};

ConnectionWatch::Told told(ConnectionWatch& watch, std::chrono::milliseconds at, bool connected, bool paired = false) {
	return watch.read({paired, connected}, zero + at);
}

bool nothing(const ConnectionWatch::Told& t) { return !t.dial && !t.connected && !t.disconnected; }

} // namespace

TEST_CASE("A first reading dials once, and no second dial comes before 31 s", "[sendspin][watch]") {
	ConnectionWatch watch(true);
	CHECK(told(watch, 0ms, false).dial);
	CHECK(nothing(told(watch, 10ms, false)));
	CHECK(nothing(told(watch, 30999ms, false)));
	CHECK(told(watch, 31000ms, false).dial);
	CHECK(nothing(told(watch, 31010ms, false)));
	CHECK(nothing(told(watch, 61999ms, false)));
	CHECK(told(watch, 62000ms, false).dial);
}

TEST_CASE("A watch that does not dial never dials", "[sendspin][watch]") {
	ConnectionWatch watch(false);
	CHECK(nothing(told(watch, 0ms, false)));
	CHECK(nothing(told(watch, 100s, false)));
	CHECK(told(watch, 200s, true).connected);
	CHECK(nothing(told(watch, 201s, false)));
	const auto gone = told(watch, 206s, false);
	CHECK(gone.disconnected);
	CHECK_FALSE(gone.dial);
}

TEST_CASE("Connected is told once", "[sendspin][watch]") {
	ConnectionWatch watch(true);
	told(watch, 0ms, false);
	CHECK(told(watch, 1s, true).connected);
	CHECK(nothing(told(watch, 2s, true)));
	CHECK(nothing(told(watch, 100s, true)));
}

TEST_CASE("False for under 5 s tells nothing, and paired changing tells connected again", "[sendspin][watch]") {
	ConnectionWatch watch(true);
	told(watch, 0ms, false);
	told(watch, 1s, true, false);
	CHECK(nothing(told(watch, 2s, false, true))); // a re-handshake: the new session's paired reads first
	CHECK(nothing(told(watch, 6999ms, false, true)));
	const auto again = told(watch, 7s, true, true);
	CHECK(again.connected);
	CHECK_FALSE(again.disconnected);
	CHECK_FALSE(again.dial);
	CHECK(nothing(told(watch, 8s, true, true)));
	// The same back, in the open, with no false at all.
	CHECK(told(watch, 9s, true, false).connected);
}

TEST_CASE("A false reading restarts its 5 s after each true", "[sendspin][watch]") {
	ConnectionWatch watch(true);
	told(watch, 0s, false);
	told(watch, 100s, true);
	CHECK(nothing(told(watch, 101s, false)));
	CHECK(nothing(told(watch, 102s, true)));
	CHECK(nothing(told(watch, 103s, false)));
	CHECK(nothing(told(watch, 107999ms, false)));
	CHECK(told(watch, 108s, false).disconnected);
}

TEST_CASE("5 s of false tells disconnected and dials, then no second dial before 31 s, one at 31 s",
          "[sendspin][watch]") {
	ConnectionWatch watch(true);
	told(watch, 0s, false);
	told(watch, 100s, true);
	CHECK(nothing(told(watch, 101s, false)));
	CHECK(nothing(told(watch, 105999ms, false)));
	const auto gone = told(watch, 106s, false);
	CHECK(gone.disconnected);
	CHECK(gone.dial);
	CHECK_FALSE(gone.connected);
	CHECK(nothing(told(watch, 106010ms, false)));
	CHECK(nothing(told(watch, 136999ms, false)));
	const auto redial = told(watch, 137s, false);
	CHECK(redial.dial);
	CHECK_FALSE(redial.disconnected);
	// It answers: connected again, and the next loss is told again.
	CHECK(told(watch, 138s, true).connected);
	CHECK(told(watch, 200s, false).dial == false);
	CHECK(told(watch, 205s, false).disconnected);
}

TEST_CASE("A connection lost soon after its dial is told at 5 s, and redialled only at 31 s", "[sendspin][watch]") {
	ConnectionWatch watch(true);
	CHECK(told(watch, 0s, false).dial);
	CHECK(told(watch, 1s, true).connected);
	CHECK(nothing(told(watch, 2s, false)));
	const auto gone = told(watch, 7s, false);
	CHECK(gone.disconnected);
	CHECK_FALSE(gone.dial);
	CHECK(nothing(told(watch, 30s, false)));
	CHECK(told(watch, 31s, false).dial);
}
