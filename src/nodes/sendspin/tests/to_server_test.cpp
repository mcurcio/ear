// The source role's audio on its way to the library (docs/design/sendspin.md sections 6 and 7): each piece's stamp comes
// from the capture anchor, however late the handoff's thread woke.
#include <array>
#include <cstdint>
#include <vector>

#include "catch2/catch.hpp"
#include "to_server.hpp"

using namespace ear::sendspin;

namespace {

constexpr std::uint32_t rate = 48000;

struct Piece {
	std::size_t bytes;
	std::int64_t stampUs;
};

class FakeSink final : public SourceSink {
public:
	void writeAudio(const std::uint8_t*, std::size_t bytes, std::int64_t stampUs) override {
		pieces.push_back({bytes, stampUs});
	}
	std::vector<Piece> pieces;
};

struct Rig {
	Rig() : toServer(rate, sink, anchor), samples(std::size_t{rate} * 2, 0.0F) {}
	void consume(std::uint32_t frames) {
		toServer.consume(std::span<const float>(samples).first(std::size_t{frames} * 2), frames);
	}
	FakeSink sink;
	Mailbox<CaptureAnchor> anchor;
	ToServer toServer;
	std::vector<float> samples;
};

} // namespace

TEST_CASE("A stamp is the anchor's time plus the frames since it, whenever consume runs", "[sendspin][to_server]") {
	Rig rig;
	rig.toServer.streaming(true);
	// The cycle whose first frame is number 480 started at 5 s. This consume ran whenever it ran: no clock is read.
	rig.anchor.publish({480, 5'000'000'000});
	rig.consume(240); // frames 0 to 239: 10 ms before the anchor
	rig.consume(240); // frames 240 to 479: 5 ms before it
	rig.consume(240); // frames 480 to 719: at it
	REQUIRE(rig.sink.pieces.size() == 3);
	CHECK(rig.sink.pieces[0].stampUs == 4'990'000);
	CHECK(rig.sink.pieces[1].stampUs == 4'995'000);
	CHECK(rig.sink.pieces[2].stampUs == 5'000'000);
	CHECK(rig.sink.pieces[2].bytes == 240 * 6);
}

TEST_CASE("A long consume goes in pieces of 100 ms, each dated from where it starts", "[sendspin][to_server]") {
	Rig rig;
	rig.toServer.streaming(true);
	rig.anchor.publish({0, 1'000'000'000});
	rig.consume(rate / 4); // 250 ms
	REQUIRE(rig.sink.pieces.size() == 3);
	CHECK(rig.sink.pieces[0].stampUs == 1'000'000);
	CHECK(rig.sink.pieces[1].stampUs == 1'100'000);
	CHECK(rig.sink.pieces[2].stampUs == 1'200'000);
	CHECK(rig.sink.pieces[0].bytes == rate / 10 * 6);
	CHECK(rig.sink.pieces[2].bytes == rate / 20 * 6);
}

TEST_CASE("Before the first anchor the library is asked to stamp it", "[sendspin][to_server]") {
	Rig rig;
	rig.toServer.streaming(true);
	rig.consume(240);
	REQUIRE(rig.sink.pieces.size() == 1);
	CHECK(rig.sink.pieces[0].stampUs == 0);
}

TEST_CASE("While nobody wants the audio it is dropped, and the frames still count for the next stamp",
          "[sendspin][to_server]") {
	Rig rig;
	rig.anchor.publish({0, 1'000'000'000});
	rig.consume(480);
	CHECK(rig.sink.pieces.empty());
	rig.toServer.streaming(true);
	rig.consume(240);
	REQUIRE(rig.sink.pieces.size() == 1);
	CHECK(rig.sink.pieces[0].stampUs == 1'010'000);
	rig.toServer.streaming(false);
	rig.consume(240);
	CHECK(rig.sink.pieces.size() == 1);
}
