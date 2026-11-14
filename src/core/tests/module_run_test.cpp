// The module-run converters' one latency rule (docs/engine.md#module-run-converters): what a module buffers beyond its
// bank, at the graph rate, in whole frames. NodeHost's latency plan enters it as `beyond` the bank for every module-run
// shape and for nothing else (the rtp type's tests carry it through a configuration).
#include <chrono>
#include <cstdint>

#include "catch2/catch.hpp"
#include "ear/core/companions.hpp"

namespace {
using namespace ear;

RtpReceiverShape receiver(std::uint32_t latencyMs) {
	return RtpReceiverShape{*RtpAddress::parse("0.0.0.0"), 5004, RtpEncoding::l24, SampleRate::fixed<48000>(),
	                        latencyMs};
}

} // namespace

TEST_CASE("A module-run converter's delay is its module's buffer at the graph rate, least and most alike, in whole "
          "frames",
          "[module_run][latency]") {
	struct Row {
		std::uint32_t graphRate;
		std::uint32_t latencyMs;
		std::uint32_t frames;
	};
	for (const Row row : {Row{48000, 100, 4800}, Row{44100, 7, 308}, Row{44100, 250, 11025}, Row{192000, 1000, 192000},
	                      Row{8000, 1, 8}, Row{768000, 1, 768}}) {
		INFO(row.latencyMs << " ms at " << row.graphRate << " Hz");
		CHECK(moduleRunDelay(receiver(row.latencyMs), FrameClock(*ear::SampleRate::of(row.graphRate))) ==
		      Delay{Latency{row.frames, row.frames}});
	}
	// The sender's rate is the module's own side: only the graph's rate counts what it buffers.
	RtpReceiverShape at44100 = receiver(100);
	at44100.rate = ear::SampleRate::fixed<44100>();
	CHECK(moduleRunDelay(at44100, FrameClock(ear::SampleRate::fixed<48000>())) == Delay{Latency{4800, 4800}});
}

TEST_CASE("A shape is module-run when it says what its module buffers: an RTP receiver's is, a card's is not",
          "[module_run]") {
	STATIC_REQUIRE(ModuleRunShape<RtpReceiverShape>);
	STATIC_REQUIRE_FALSE(ModuleRunShape<SoundCardShape>);
}
