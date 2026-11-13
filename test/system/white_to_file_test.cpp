// The owner's end-to-end test: the real out/ear runs a graph file
// that routes white noise into one channel of a file node, is stopped with SIGTERM and leaves a
// finished WAV file with the noise on that channel only. Everything it prints is a record
// (docs/output-records.md), seq from 1 with no gap.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <filesystem>
#include <string>

#include "ear_system.hpp"

namespace {

using namespace ear::test;

/// Every line ear printed is one record of test/protocol/output-records.json's envelope, in seq order with no gap.
void checkAllRecords(const ChildProcess& ear) {
	CHECK(ear.plainLines().empty());
	REQUIRE_FALSE(ear.records().empty());
	std::uint64_t seq = 0;
	for (const json& record : ear.records()) {
		INFO(record.dump());
		CHECK(record.value("v", 0) == 1);
		CHECK(record.value("seq", std::uint64_t{0}) == ++seq);
		REQUIRE(record.contains("t"));
		CHECK(record["t"].get<std::string>().ends_with("Z"));
		CHECK(record["t"].get<std::string>().size() == std::string("2026-11-07T17:42:03.512094Z").size());
		CHECK(record.contains("kind"));
		CHECK_FALSE(record.contains("dropped"));
	}
}

/// Runs ear on a graph routing noise to rec:`to` until the WAV holds a second of it, stops it with SIGTERM and checks
/// the WAV: noise on channel `noisy`, exact zeros on the other.
void recordNoise(const std::string& to, int noisy) {
	TempDirectory directory;
	writeGraphFile(directory, noiseToRec(to));
	ChildProcess ear(EAR_BINARY, earArguments(directory));

	const bool linked = ear.waitForLine(linkedLine("noise:noise.mono", "rec:" + to), deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(linked);
	// The recording window: until the WAV holds a second of noise; the checks below need at least half of it.
	CHECK(waitForNoiseSecond(directory.recordings() / "rec.wav", noisy, 48000));
	ear.signal(SIGTERM);
	const auto exitStatus = waitForStop(ear);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(exitStatus == 0);
	CHECK(ear.hasLine("node rec removed dropped=0"));
	checkAllRecords(ear);
	checkNoiseOnOneChannel(directory.recordings() / "rec.wav", noisy, 48000);
}

} // namespace

TEST_CASE("White noise routed to record.left lands on left only", "[system]") { recordNoise("record.left", 0); }

TEST_CASE("White noise routed to record.right lands on right only", "[system]") { recordNoise("record.right", 1); }

TEST_CASE("A stop ends with the record ear: stopped, after every node is removed, and ear exits 0", "[system]") {
	TempDirectory directory;
	writeGraphFile(directory, noiseToRec());
	ChildProcess ear(EAR_BINARY, earArguments(directory));
	const bool linked = ear.waitForLine(linkedLine("noise:noise.mono", "rec:record.left"), deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(linked);
	ear.signal(SIGTERM);
	// ear's own word that its stop is done, waited for alone: the process's end after it is the kernel's teardown,
	// which a starved host stretches past any bound a test could judge.
	REQUIRE(ear.waitForLine(std::string(stoppedLine), deadline));
	CHECK(waitForStop(ear) == 0);
	checkAllRecords(ear);
	const auto& records = ear.records();
	const auto at = [&](const std::string& msg) {
		return std::ranges::find_if(records, [&](const json& record) { return record["msg"] == msg; }) -
		       records.begin();
	};
	const auto stopped = at(std::string(stoppedLine));
	CHECK(at("node noise removed dropped=0") < stopped);
	CHECK(at("node rec removed dropped=0") < stopped);
	CHECK(records[static_cast<std::size_t>(stopped)]["kind"] == "exit");
	CHECK(records[static_cast<std::size_t>(stopped)]["status"] == 0);
	CHECK(records[static_cast<std::size_t>(stopped)]["reason"] == "stopped");
	CHECK(headerFinished(directory.recordings() / "rec.wav"));
}

TEST_CASE("A recordings folder ear cannot make stops ear with exit status 1", "[system]") {
	TempDirectory directory;
	writeGraphFile(directory, noiseToRec());
	const auto recordings = directory.path() / "missing" / "recordings";
	ChildProcess ear(EAR_BINARY, {"--config-dir", directory.config().string(), "--control-port", "0",
	                              "--recordings-dir", recordings.string()});
	const auto exitStatus = ear.waitForExit(deadline);
	INFO("ear printed:\n" << ear.output());
	CHECK(exitStatus == 1);
	CHECK(ear.output().find("recordings folder " + recordings.string() + ": cannot create") != std::string::npos);
	checkAllRecords(ear);
	REQUIRE_FALSE(ear.records().empty());
	const json& last = ear.records().back();
	CHECK(last["kind"] == "exit");
	CHECK(last["status"] == 1);
	CHECK(last["reason"].get<std::string>().starts_with("recordings folder " + recordings.string()));
	CHECK_FALSE(std::filesystem::exists(directory.path() / "missing"));
}
