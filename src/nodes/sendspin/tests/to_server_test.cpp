// The source role's audio on its way to the library (docs/node-types.md#threads): each piece's stamp comes from its own
// chunk's cycle start, however late the sink's thread woke.
#include <array>
#include <cstdint>
#include <vector>

#include "catch2/catch.hpp"
#include "to_server.hpp"

using namespace ear::nodes::sendspin;

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
	Rig() : toServer(ear::SampleRate::fixed<rate>(), sink), samples(std::size_t{rate} * 2, 0.0F) {}
	/// A chunk of `frames` whose cycle started at `startRawNs`, as the sink hands it.
	void consume(std::uint32_t frames, std::int64_t startRawNs) {
		toServer.consume(ear::Chunk{.interleaved = std::span<const float>(samples).first(std::size_t{frames} * 2),
		                            .frames = frames,
		                            .position = 0,
		                            .startNs = startRawNs - 7, // another clock: the consumer must not read it
		                            .startRawNs = startRawNs,
		                            .upstreamFrames = 0,
		                            .causes = {},
		                            .lostFrames = 0});
	}
	FakeSink sink;
	ToServer toServer;
	std::vector<float> samples;
};

} // namespace

TEST_CASE("A stamp is its chunk's cycle start on the raw clock, whenever consume runs", "[sendspin][to_server]") {
	Rig rig;
	rig.toServer.streaming(true);
	// Each chunk's cycle started when its stamp says; these consumes ran whenever they ran: no clock is read.
	rig.consume(240, 4'990'000'000);
	rig.consume(240, 4'995'000'000);
	rig.consume(240, 5'000'000'000);
	REQUIRE(rig.sink.pieces.size() == 3);
	CHECK(rig.sink.pieces[0].stampUs == 4'990'000);
	CHECK(rig.sink.pieces[1].stampUs == 4'995'000);
	CHECK(rig.sink.pieces[2].stampUs == 5'000'000);
	CHECK(rig.sink.pieces[2].bytes == 240 * 6);
}

TEST_CASE("A long chunk goes in pieces of 100 ms, each dated from where it starts in the chunk",
          "[sendspin][to_server]") {
	Rig rig;
	rig.toServer.streaming(true);
	rig.consume(rate / 4, 1'000'000'000); // 250 ms
	REQUIRE(rig.sink.pieces.size() == 3);
	CHECK(rig.sink.pieces[0].stampUs == 1'000'000);
	CHECK(rig.sink.pieces[1].stampUs == 1'100'000);
	CHECK(rig.sink.pieces[2].stampUs == 1'200'000);
	CHECK(rig.sink.pieces[0].bytes == rate / 10 * 6);
	CHECK(rig.sink.pieces[2].bytes == rate / 20 * 6);
}

TEST_CASE("While nobody wants the audio it is dropped, and the next chunk wanted is dated by its own start",
          "[sendspin][to_server]") {
	Rig rig;
	rig.consume(480, 1'000'000'000);
	CHECK(rig.sink.pieces.empty());
	rig.toServer.streaming(true);
	rig.consume(240, 1'010'000'000);
	REQUIRE(rig.sink.pieces.size() == 1);
	CHECK(rig.sink.pieces[0].stampUs == 1'010'000);
	rig.toServer.streaming(false);
	rig.consume(240, 1'015'000'000);
	CHECK(rig.sink.pieces.size() == 1);
}
