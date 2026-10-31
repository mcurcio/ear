// The owner's end-to-end test (docs/design/m1-white-to-file.md section 3, S6 criteria 1-4; ported to
// ear.conf by docs/design/knobly-config.md, S5 criterion T9): the real out/ear runs a graph file
// that routes white noise into one channel of a file node, is stopped with SIGTERM and leaves a
// finished WAV file with the noise on that channel only.
#include <chrono>
#include <csignal>
#include <filesystem>
#include <string>
#include <thread>

#include "ear_system.hpp"

namespace {

using namespace ear::test;

/// Runs ear on a graph routing noise to rec:`to` for about a second after the route is linked,
/// stops it with SIGTERM and checks the WAV: noise on channel `noisy`, exact zeros on the other.
void recordNoise(const std::string& to, int noisy) {
	TempDirectory directory;
	writeGraphFile(directory, noiseToRec(to));
	ChildProcess ear(EAR_BINARY, earArguments(directory));

	const bool linked = ear.waitForLine(linkedLine("noise:MONO", "rec:" + to), deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(linked);
	// The recording window the criterion asks for; the checks below need at least half of it.
	std::this_thread::sleep_for(1s);
	ear.signal(SIGTERM);
	const auto exitStatus = ear.waitForExit(deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(exitStatus == 0);
	CHECK(ear.hasLine("node rec stopped dropped=0"));
	checkNoiseOnOneChannel(directory.recordings() / "rec.wav", noisy, 48000);
}

} // namespace

TEST_CASE("White noise routed to file:FL lands on FL only", "[system]") {
	recordNoise("FL", 0);
}

TEST_CASE("White noise routed to file:FR lands on FR only", "[system]") {
	recordNoise("FR", 1);
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
	CHECK_FALSE(std::filesystem::exists(directory.path() / "missing"));
}
