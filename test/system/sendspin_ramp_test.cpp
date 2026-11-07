// The judge of the sendspin system tests' ramp, `checkRamp`, on hand-made frames: no ear, no server. What it may pass is the
// library's correction of a clock error and nothing else (docs/design/sendspin.md erratum 42).
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "sendspin_system.hpp"

namespace {

using namespace ear::test;

/// A frame in units of 1/32768, as `checkRamp`'s ramp is: {low word as a signed sample, high word as one}.
using Units = std::array<float, 2>;

Units whole(std::uint32_t index) {
	return {static_cast<float>(static_cast<std::int16_t>(index & 0xFFFFU)),
	        static_cast<float>(static_cast<std::int16_t>(index >> 16U))};
}

/// sync_task.cpp's soft_sync_insert_frame and soft_sync_drop_frame on two ramp frames, by its own steps: Q31 from audio_utils.h,
/// `a / 2 + b / 2`, and 24 bits out, then pcm.hpp's division by 2^23. Then in units of 1/32768.
Units libraryBlend(std::uint32_t index) {
	const auto channel = [](float a, float b) {
		const auto q31 = [](float units) { return static_cast<std::int32_t>(units) * 65536; };
		return static_cast<float>((q31(a) / 2 + q31(b) / 2) >> 8) / 8388608.0F * 32768.0F;
	};
	const Units a = whole(index);
	const Units b = whole(index + 1);
	return {channel(a[0], b[0]), channel(a[1], b[1])};
}

Wav wavOf(const std::vector<Units>& frames) {
	Wav wav;
	wav.info.frames = static_cast<sf_count_t>(frames.size());
	wav.info.channels = 2;
	for (const Units& frame : frames) {
		wav.samples.push_back(frame[0] / 32768.0F);
		wav.samples.push_back(frame[1] / 32768.0F);
	}
	return wav;
}

RampCheck check(const std::vector<Units>& frames) { return checkRamp(wavOf(frames)); }

/// Ramp frames `from` up to and not including `to`.
std::vector<Units> run(std::uint32_t from, std::uint32_t to) {
	std::vector<Units> frames;
	for (std::uint32_t index = from; index < to; ++index) frames.push_back(whole(index));
	return frames;
}

std::vector<Units> joined(std::initializer_list<std::vector<Units>> parts) {
	std::vector<Units> frames;
	for (const auto& part : parts) frames.insert(frames.end(), part.begin(), part.end());
	return frames;
}

/// n-1, the library's blend of n-1 and n, n.
std::vector<Units> inserted(std::uint32_t n) { return {whole(n - 1), libraryBlend(n - 1), whole(n)}; }
/// n-1, the library's blend of n and n+1, n+2.
std::vector<Units> dropped(std::uint32_t n) { return {whole(n - 1), libraryBlend(n), whole(n + 2)}; }

void checkOneBlend(const RampCheck& ramp) {
	INFO(ramp);
	CHECK(ramp.blends == 1);
	CHECK(ramp.badFrames == 0);
	CHECK(ramp.badSteps == 0);
}

void checkNoBlend(const RampCheck& ramp) {
	INFO(ramp);
	CHECK(ramp.blends == 0);
	CHECK(ramp.badFrames == 1);
}

} // namespace

TEST_CASE("A clean ramp is all exact frames with no blend", "[system][sendspin][ramp]") {
	const RampCheck ramp = check(run(0, 1000));
	INFO(ramp);
	CHECK(ramp.rampFrames == 999); // index 0 is silence
	CHECK(ramp.blends == 0);
	CHECK(ramp.badFrames == 0);
	CHECK(ramp.badSteps == 0);
	CHECK(ramp.firstIndex == 1);
	CHECK(ramp.lastIndex == 999);
	CHECK(ramp.lastRunFrames == 999);
}

TEST_CASE("A whole frame that is not the next one, by a step no skip makes, is a bad step",
          "[system][sendspin][ramp]") {
	const RampCheck ramp = check({whole(100), whole(103), whole(104)});
	INFO(ramp);
	CHECK(ramp.badSteps == 1);
	CHECK(ramp.lastRunFrames == 2);
}

TEST_CASE("A skip's block multiple is still allowed", "[system][sendspin][ramp]") {
	const RampCheck ramp = check({whole(100), whole(101 + blockSize), whole(102 + blockSize)});
	INFO(ramp);
	CHECK(ramp.badSteps == 0);
	CHECK(ramp.lastRunFrames == 3);
}

TEST_CASE("The inserted frame as CI saw it passes", "[system][sendspin][ramp]") {
	// CI's own frames, (12798,1) (12798.5,1) (12799,1).
	const RampCheck ramp = check({{12798.0F, 1.0F}, {12798.5F, 1.0F}, {12799.0F, 1.0F}});
	checkOneBlend(ramp);
	CHECK(ramp.rampFrames == 2);
	CHECK(ramp.lastRunFrames == 2);
	CHECK(ramp.firstIndex == 65536 + 12798);
	CHECK(ramp.lastIndex == 65536 + 12799);
}

TEST_CASE("A dropped frame passes, its step of 3 neither a bad step nor the end of a run", "[system][sendspin][ramp]") {
	const RampCheck ramp = check(joined({run(90, 100), {libraryBlend(100)}, run(102, 110)}));
	checkOneBlend(ramp);
	CHECK(ramp.rampFrames == 18);
	CHECK(ramp.lastRunFrames == 18);
	CHECK(ramp.firstIndex == 90);
	CHECK(ramp.lastIndex == 109);
}

TEST_CASE("A blend across the low word's wrap passes as the library makes it, and not as a plain k + 0.5",
          "[system][sendspin][ramp]") {
	SECTION("the helper below is the library's arithmetic, in numbers") {
		CHECK(libraryBlend(1000) == Units{1000.5F, 0.0F});
		CHECK(libraryBlend(0x7FFF) == Units{-0.5F, 0.0F});         // 32767 and -32768
		CHECK(libraryBlend(0xFFFF) == Units{-0.5F, 0.5F});         // -1 and 0, 0 and 1
		CHECK(libraryBlend(0x1FFFF) == Units{-0.5F, 1.5F});        // -1 and 0, 1 and 2
		CHECK(libraryBlend(0x7FFF7FFF) == Units{-0.5F, 32767.0F}); // the high word holds
	}
	SECTION("the low word's sign, inserted") { checkOneBlend(check(inserted(0x8000))); }
	SECTION("the low word's sign, dropped") { checkOneBlend(check(dropped(0x7FFF))); }
	SECTION("the low word's end, inserted, both channels fractional") { checkOneBlend(check(inserted(0x10000))); }
	SECTION("the low word's end, dropped, both channels fractional") { checkOneBlend(check(dropped(0xFFFF))); }
	SECTION("the low word's sign as k + 0.5 is refused") {
		checkNoBlend(check({whole(0x7FFF), {32767.5F, 0.0F}, whole(0x8000)}));
	}
	SECTION("the low word's end with the high word whole is refused") {
		checkNoBlend(check({whole(0xFFFF), {-0.5F, 0.0F}, whole(0x10000)}));
		checkNoBlend(check({whole(0xFFFF), {-0.5F, 1.0F}, whole(0x10000)}));
	}
	SECTION("the high word fractional anywhere else is refused") {
		checkNoBlend(check({whole(1000), {1000.5F, 0.5F}, whole(1001)}));
	}
}

TEST_CASE("Every ramp frame's blend passes, inserted and dropped", "[system][sendspin][ramp]") {
	std::size_t failures = 0;
	for (std::uint32_t n = 2; n < 0x10010; ++n) {
		for (const RampCheck& ramp : {check(inserted(n)), check(dropped(n))}) {
			if (ramp.blends != 1 || ramp.badFrames != 0 || ramp.badSteps != 0) ++failures;
		}
	}
	CHECK(failures == 0);
}

TEST_CASE("Any other fraction is a bad frame, whatever its neighbours", "[system][sendspin][ramp]") {
	SECTION("a quarter") { checkNoBlend(check({whole(1000), {1000.25F, 0.0F}, whole(1001)})); }
	SECTION("three quarters") { checkNoBlend(check({whole(1000), {1000.75F, 0.0F}, whole(1001)})); }
	SECTION("the last bit a 24-bit sample has") {
		checkNoBlend(check({whole(1000), {1000.5F + 1.0F / 256.0F, 0.0F}, whole(1001)}));
	}
	SECTION("the right channel alone") { checkNoBlend(check({whole(1000), {1000.0F, 0.5F}, whole(1001)})); }
}

TEST_CASE("A blend's own fraction with the wrong step around it is a bad frame", "[system][sendspin][ramp]") {
	const auto badBoth = [](const RampCheck& ramp) {
		checkNoBlend(ramp);
		CHECK(ramp.badSteps == 1);
		CHECK(ramp.lastRunFrames == 1);
	};
	SECTION("a step of 2") { badBoth(check({whole(1000), libraryBlend(1001), whole(1002)})); }
	SECTION("a step of 4") { badBoth(check({whole(1000), libraryBlend(1001), whole(1004)})); }
	SECTION("a step of a block and 3") { badBoth(check({whole(1000), libraryBlend(1001), whole(1003 + blockSize)})); }
	SECTION("backward") { badBoth(check({whole(1000), libraryBlend(1000), whole(999)})); }
}

TEST_CASE("A blend of two other frames, or beside another off-ramp frame, is a bad frame", "[system][sendspin][ramp]") {
	SECTION("an insert's neighbours around another place's blend") {
		checkNoBlend(check({whole(1000), libraryBlend(1005), whole(1001)}));
	}
	SECTION("a drop's neighbours around an insert's blend") {
		const RampCheck ramp = check({whole(1000), libraryBlend(1000), whole(1003)});
		checkNoBlend(ramp);
		CHECK(ramp.badSteps == 1);
	}
	SECTION("an insert's neighbours around a drop's blend") {
		checkNoBlend(check({whole(1000), libraryBlend(1001), whole(1001)}));
	}
	SECTION("two blends side by side") {
		const RampCheck ramp = check({whole(1000), libraryBlend(1000), libraryBlend(1001), whole(1002)});
		INFO(ramp);
		CHECK(ramp.blends == 0);
		CHECK(ramp.badFrames == 2);
	}
}

TEST_CASE("A blend on a run's edge passes on the neighbour it has", "[system][sendspin][ramp]") {
	SECTION("first, a drop of the first chunk's first frame") {
		checkOneBlend(check({libraryBlend(1000), whole(1002)}));
	}
	SECTION("first, a drop of the ramp's own frame 0") {
		checkOneBlend(check({whole(0), libraryBlend(0), whole(2), whole(3)}));
	}
	SECTION("first, after silence inside the file") {
		checkOneBlend(check({whole(1001 - blockSize), whole(0), libraryBlend(1000), whole(1002)}));
	}
	SECTION("last, an insert with its tail cut") { checkOneBlend(check({whole(1000), libraryBlend(1000)})); }
	SECTION("last, a drop with its tail cut") { checkOneBlend(check({whole(1000), libraryBlend(1001)})); }
	SECTION("last, before silence inside the file") {
		checkOneBlend(check({whole(1000), libraryBlend(1000), whole(0), whole(1001 + blockSize)}));
	}
}

TEST_CASE("A blend on a run's edge with the wrong neighbour, or none, is a bad frame", "[system][sendspin][ramp]") {
	SECTION("first in an insert's shape, which leaves its frame before it") {
		checkNoBlend(check({libraryBlend(1000), whole(1001)}));
	}
	SECTION("first, its next frame not the drop's") { checkNoBlend(check({libraryBlend(1000), whole(1003)})); }
	SECTION("last, its frame before neither shape's") { checkNoBlend(check({whole(1000), libraryBlend(1002)})); }
	SECTION("alone") { checkNoBlend(check({libraryBlend(1000)})); }
	SECTION("the wrong fraction at the start") { checkNoBlend(check({{1000.25F, 0.0F}, whole(1002)})); }
	SECTION("the wrong fraction at the end") { checkNoBlend(check({whole(1000), {1000.25F, 0.0F}})); }
}

TEST_CASE("Blends past one per chunk are bad frames, up to the limit they are not", "[system][sendspin][ramp]") {
	// A correction every `spacing` frames, each a drop, over a run of `count` of them.
	const auto correctedEvery = [](std::size_t spacing, std::size_t count) {
		std::vector<Units> frames;
		std::uint32_t index = 1;
		for (std::size_t correction = 0; correction < count; ++correction) {
			for (std::size_t frame = 0; frame + 1 < spacing; ++frame) frames.push_back(whole(index++));
			frames.push_back(libraryBlend(index));
			index += 2;
		}
		frames.push_back(whole(index));
		return frames;
	};
	SECTION("one per chunk, as the library may") {
		const RampCheck ramp = check(correctedEvery(chunkFrames, 16));
		INFO(ramp);
		CHECK(ramp.blends == 16);
		CHECK(ramp.badFrames == 0);
		CHECK(ramp.badSteps == 0);
	}
	SECTION("what was seen under a forced shift, 4 in 1.5 s, is far inside it") {
		CHECK(4 <= blendLimit(graphFrequency * 3 / 2));
		CHECK(blendLimit(graphFrequency * 3 / 2) == 17);
	}
	SECTION("exactly at the limit, and one past it") {
		const std::size_t spacing = 1000;
		const RampCheck atLimit = check(correctedEvery(spacing, 2)); // 2001 frames: no whole chunk, so 2
		const RampCheck past = check(correctedEvery(spacing, 3));    // 3001 frames: still 2
		INFO(atLimit << "; " << past);
		CHECK(atLimit.blends == 2);
		CHECK(atLimit.badFrames == 0);
		CHECK(past.blends == 3);
		CHECK(past.badFrames == 1);
	}
}
