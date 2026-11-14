// The judge of the sendspin system tests' ramp, `checkRamp`, on hand-made frames: no ear, no server. What it may pass
// is the library's correction of a clock error and nothing else (docs/node-types.md#library-facts-and-carried-patches).
#include <array>
#include <string>
#include <format>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
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

TEST_CASE("A skip's block multiple is a skip, its blocks counted for ear's records to account for",
          "[system][sendspin][ramp]") {
	const RampCheck ramp =
		check({whole(100), whole(101 + blockSize), whole(102 + blockSize), whole(103 + 4 * blockSize)});
	INFO(ramp);
	CHECK(ramp.badSteps == 0);
	CHECK(ramp.lastRunFrames == 4);
	CHECK(ramp.skippedBlocks == 4); // one, then three: what the test's ear must have lost (LostCycles::budget)
}

TEST_CASE("A skip a frame either way of whole blocks is a bad step, as is a step back of whole blocks",
          "[system][sendspin][ramp]") {
	// A running block jumps by whole blocks exactly (spike S6 in spikes_test.cpp); and the index is 32 bits, so a step
	// back must not pass as a skip round it.
	for (const std::int64_t step : {std::int64_t{blockSize}, std::int64_t{blockSize} + 2, 2 * std::int64_t{blockSize},
	                                -std::int64_t{blockSize} + 1}) {
		const auto next = static_cast<std::uint32_t>(1000 + step);
		const RampCheck ramp = check({whole(1000), whole(next), whole(next + 1)});
		INFO(step << ": " << ramp);
		CHECK(ramp.badSteps == 1);
	}
}

TEST_CASE("A loss the library logged accounts for one forward step, by its size where it fits and apart where not",
          "[system][sendspin][ramp]") {
	// 5312 us late at 48 kHz: 254.976 frames, of which the library drops the whole 254.
	const auto dropped = [](std::uint32_t frames, std::vector<std::int64_t> logged) {
		return checkRamp(wavOf({whole(100), whole(101 + frames), whole(102 + frames)}), std::move(logged));
	};
	const auto wholeChunks = [](std::uint32_t frames) { return static_cast<std::uint32_t>(2 * chunkFrames + frames); };
	SECTION("its drop, and whole chunks before it") {
		for (const std::uint32_t frames : {std::uint32_t{254}, wholeChunks(254)}) {
			const RampCheck ramp = dropped(frames, {-5312});
			INFO(frames << ": " << ramp);
			CHECK(ramp.badSteps == 0);
			CHECK(ramp.hardSyncSteps == 1);
			CHECK(ramp.stepsInLoss == 0);
		}
	}
	SECTION("a loss that began with silence, no more than the silence") {
		const RampCheck ramp = dropped(254, {5312});
		INFO(ramp);
		CHECK(ramp.hardSyncSteps == 1);
		CHECK(ramp.stepsInSilentLoss == 1);
	}
	SECTION("more than its line says: the loss's later hard syncs, counted apart") {
		for (const std::int64_t us : {std::int64_t{-5312}, std::int64_t{5312}}) {
			const RampCheck ramp = dropped(wholeChunks(255), {us});
			INFO(us << ": " << ramp);
			CHECK(ramp.badSteps == 0);
			CHECK(ramp.hardSyncSteps == 0);
			CHECK(ramp.stepsInLoss == 1);
		}
	}
	SECTION("no line") { CHECK(dropped(254, {}).badSteps == 1); }
	SECTION("one line, two steps") {
		const RampCheck ramp =
			checkRamp(wavOf({whole(100), whole(355), whole(356), whole(610)}), std::vector<std::int64_t>{-5312});
		CHECK(ramp.hardSyncSteps == 1);
		CHECK(ramp.badSteps == 1);
	}
	SECTION("a step back, line or none") {
		const RampCheck ramp =
			checkRamp(wavOf({whole(1000), whole(900), whole(901)}), std::vector<std::int64_t>{-5312});
		CHECK(ramp.badSteps == 1);
		CHECK(ramp.stepsInLoss == 0);
	}
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

namespace {

/// A counter record at `ms` after a fixed instant: a graph record of `cycles` cut, or `node`'s of `cycles` missed.
json lossRecord(std::int64_t ms, std::uint64_t cycles, const std::string& node = "") {
	const RecordTime at = RecordTime{std::chrono::seconds(1'800'000'000)} + std::chrono::milliseconds(ms);
	const auto day = std::chrono::floor<std::chrono::days>(at);
	const std::string t = std::format("{:%F}T{:%T}Z", std::chrono::sys_days(day), at - day);
	if (node.empty()) return {{"code", "ear.graph-lost-cycles"}, {"t", t}, {"cycles", cycles}};
	return {{"code", "ear.node-missed-cycles"}, {"t", t}, {"node", node}, {"missed", cycles}};
}

/// A skip of `blocks` whose cycles were lost between `from` and `to` ms after the same instant.
SeenSkip skipAt(std::int64_t from, std::int64_t to, std::uint64_t blocks = 1) {
	const RecordTime at{std::chrono::seconds(1'800'000'000)};
	return {blocks, at + std::chrono::milliseconds(from), at + std::chrono::milliseconds(to), "the skip"};
}

} // namespace

TEST_CASE("A skip is paid by the first record written after its cycles were lost, and by no other",
          "[system][sendspin][ramp][skips]") {
	// The record that covers the loss: the first graph record after it.
	CHECK(unpaidSkipsOf({lossRecord(-3000, 9), lossRecord(300, 1)}, {skipAt(0, 100)}).empty());
	// A record before the loss covers cuts before it, however many it has to spare.
	CHECK(unpaidSkipsOf({lossRecord(-300, 9)}, {skipAt(0, 100)}).size() == 1);
	// A record after the first one after the loss covers later cuts, however many it has to spare.
	const auto later = unpaidSkipsOf({lossRecord(300, 1), lossRecord(2000, 9)}, {skipAt(0, 100, 2)});
	REQUIRE(later.size() == 1);
	CHECK(later[0] == "the skip: 1 of its 2 blocks paid by no record that covers it");
	// Each record pays at most its own cycles, across skips.
	CHECK(unpaidSkipsOf({lossRecord(300, 1)}, {skipAt(0, 100), skipAt(150, 200)}).size() == 1);
	// A node's records pay too, each kind its own first record after the loss.
	CHECK(unpaidSkipsOf({lossRecord(300, 1), lossRecord(400, 1, "rec")}, {skipAt(0, 100, 2)}).empty());
	// Within the window the loss may be anywhere, so each record written in it, and the first after it, may pay.
	CHECK(unpaidSkipsOf({lossRecord(50, 1), lossRecord(600, 1)}, {skipAt(0, 100, 2)}).empty());
	// A first record after the window past the lag cannot be the record of that loss, and says so.
	const auto past = unpaidSkipsOf({lossRecord(12'000, 9)}, {skipAt(0, 100)});
	REQUIRE(past.size() == 1);
	CHECK(past[0] == "the skip: 1 of its 1 blocks paid by no record that covers it; the first graph record after it "
	                 "came past the graph record's lag");
}

namespace {

/// A ramp the file node recorded across one stall of the whole graph past the sendspin node's feed, its frame 0 at the
/// fixed instant: a second of ramp; the stall, `stallBlocks` blocks of the WAV's time lost, of which the node skipped
/// the 8 its feed held (docs/node-types.md#clock); 20 blocks of what the feed held, then 64 zeros (the feed dry); then
/// the library's one correction of the rest and of the zeros, a hard sync it logged. Then, `dropAfterMs` after the
/// stall ended, one block dropped with no cut behind it (a product bug), unless `dropAfterMs` is negative; then half a
/// second more.
struct StalledRamp {
	Wav wav;
	std::vector<std::int64_t> lostSyncsUs;
	std::int64_t stallEndMs; // when the stall ended, ms after the instant
};

StalledRamp stalledRamp(std::uint32_t stallBlocks, std::int64_t dropAfterMs) {
	constexpr std::uint32_t first = 1000; // index 0 is silent
	constexpr std::uint32_t feedBlocks = 8;
	constexpr std::uint32_t zeros = 64;
	const std::uint32_t second = graphFrequency;
	const std::uint32_t stall = stallBlocks * blockSize;
	std::vector<Units> frames = run(first, first + second);
	std::uint32_t index = first + second + feedBlocks * blockSize; // the node's skip of what its feed held
	frames = joined({frames, run(index, index + 20 * blockSize)});
	index += 20 * blockSize;
	frames.insert(frames.end(), zeros, Units{0.0F, 0.0F});
	index += stall - feedBlocks * blockSize + zeros; // the library's correction: the rest of the stall, and the zeros
	if (dropAfterMs >= 0) {
		const auto upTo = static_cast<std::uint32_t>(second + dropAfterMs * graphFrequency / 1000 - frames.size());
		frames = joined({frames, run(index, index + upTo)});
		index += upTo + blockSize; // one block dropped
	}
	frames = joined({frames, run(index, index + second / 2)});
	return {wavOf(frames), {-60'000}, 1000 + static_cast<std::int64_t>(stall) * 1000 / graphFrequency};
}

} // namespace

// After a stall past the sendspin node's feed the WAV loses the whole stall, but the node skips only what its feed held
// and the library corrects the rest (docs/node-types.md#clock). Each loss is taken as the time the WAV lost there
// (rampLosses), so the stall's record pays the stall whole with nothing to spare, and a block dropped after it with no
// cut behind it is paid by nothing: deny by default (docs/testing.md#the-counters-own-word). Revert proof: losses from
// the skips alone (rampLosses counting only each skip's own blocks) put the drop's window early by the rest of the
// stall, and the stall's record pays the drop from its spare cycles: the unpaid checks below fail.
TEST_CASE("A block dropped after a stall past the feed is paid by no record of the stall, and the stall is paid whole",
          "[system][sendspin][ramp][skips]") {
	const RecordTime at{std::chrono::seconds(1'800'000'000)};
	constexpr std::uint32_t stallBlocks = 180; // 960 ms
	for (const std::int64_t dropAfterMs : {200, 500, 900}) {
		INFO("the block dropped " << dropAfterMs << " ms after the stall");
		const StalledRamp stalled = stalledRamp(stallBlocks, dropAfterMs);
		const RampCheck ramp = checkRamp(stalled.wav, stalled.lostSyncsUs);
		INFO(ramp);
		REQUIRE(ramp.badSteps == 0);
		REQUIRE(ramp.hardSyncSteps == 1);
		REQUIRE(ramp.skips.size() == 2);
		// The stall's record, written once its cuts held still, a little after it ended.
		const json stallRecord = lossRecord(stalled.stallEndMs + 150, stallBlocks);
		const std::vector<SeenSkip> seen = rampSkipsSeen(at, ramp);
		REQUIRE(seen.size() == 2);
		const auto unpaid = unpaidSkipsOf({stallRecord}, seen);
		INFO(json(unpaid).dump());
		CHECK(unpaid.size() == 1);
		if (!unpaid.empty()) CHECK(unpaid[0].starts_with("a skip of 1 blocks at frame "));
		// A red carries each unpaid skip's evidence: its blocks and charge, the drift in its window, whether the feed ran
		// dry there, and the library's sync lines in its due window. The library's line came as the stall ended.
		const json lostSync = {
			{"kind", "log"}, {"t", lossRecord(stalled.stallEndMs + 50, 0)["t"]}, {"text", "Lost sync (-60000us off)"}};
		const auto told = unpaidRampSkipsOf({stallRecord, lostSync}, at, ramp);
		REQUIRE(told.size() == 1);
		CHECK(told[0].find("evidence: skipped 1 blocks, charged 1 blocks (256 frames)") != std::string::npos);
		CHECK(told[0].find("zeros: none (the feed never ran dry there)") != std::string::npos);
		CHECK(told[0].find("sync lines: none") != std::string::npos);
		// Unpaid, the stall shows its whole charge, the zeros of the feed running dry and the library's line.
		const auto stallTold = unpaidRampSkipsOf({lostSync}, at, ramp);
		REQUIRE(stallTold.size() == 2);
		INFO(stallTold[0]);
		CHECK(stallTold[0].find("evidence: skipped 8 blocks, charged 180 blocks") != std::string::npos);
		CHECK(stallTold[0].find(" 64@") != std::string::npos);
		CHECK(stallTold[0].find("\"Lost sync (-60000us off)\"") != std::string::npos);
		// With a record of its own the dropped block is paid, as a cut would be.
		const json dropRecord = lossRecord(stalled.stallEndMs + dropAfterMs + 150, 1);
		CHECK(unpaidSkipsOf({stallRecord, dropRecord}, seen).empty());
		// The stall is one loss of its whole length, the skip's 8 blocks and the library's correction of the rest.
		CHECK(seen[0].blocks == stallBlocks);
	}
	// With nothing dropped, the stall alone is paid by its record.
	const StalledRamp stalled = stalledRamp(stallBlocks, -1);
	const RampCheck ramp = checkRamp(stalled.wav, stalled.lostSyncsUs);
	REQUIRE(ramp.skips.size() == 1);
	CHECK(unpaidSkipsOf({lossRecord(stalled.stallEndMs + 150, stallBlocks)}, rampSkipsSeen(at, ramp)).empty());
}
