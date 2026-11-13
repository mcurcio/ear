// The diagnostics counters reported end to end (docs/engine.md#reporting-the-counters): a stall of the real out/ear,
// made by stopping the whole process, is one ear.graph-lost-cycles record on standard output however many cycles it
// cut, and the control port's snapshot and counters.changed carry the same counts. (Shared helpers: ear_system.hpp.)

#include <chrono>
#include <csignal>
#include <cstdint>
#include <string>
#include <thread>

#include "ear_system.hpp"

namespace {

using namespace ear::test;

/// How long the test holds ear stopped: every thread of it, the data thread among them, as a host that starves it does.
constexpr auto stallFor = 500ms;
/// The cycles that stall spans at the graph's 48 kHz and block of 256: 93.75.
constexpr std::uint64_t stallCycles = 93;

bool isGraphRecord(const json& record) { return record.value("code", "") == "ear.graph-lost-cycles"; }

std::uint64_t number(const json& object, const char* key) { return object.at(key).get<std::uint64_t>(); }

} // namespace

TEST_CASE("A stall is one ear.graph-lost-cycles record however many cycles it cut, and the control port carries its "
          "counts",
          "[system][counters]") {
	TempDirectory directory;
	writeGraphFile(directory, noiseToRec());
	Ear ear(directory, {linkedLine("noise:noise.mono", "rec:record.left")});
	ChildProcess& process = ear.process();
	Rpc rpc(ear.port());

	// The snapshot: the feature, the graph's counters and each node's.
	const json snapshot = rpc.call("graph.subscribe")["result"];
	INFO("the snapshot: " << snapshot.dump());
	CHECK(snapshot["features"] == json::array({"lifecycle", "counters"}));
	REQUIRE(snapshot.contains("counters"));
	CHECK(number(snapshot["counters"], "cycles") > 0);
	REQUIRE(snapshot["nodes"].size() == 2);
	for (const json& node : snapshot["nodes"])
		for (const char* counter : {"missedCycles", "overruns", "repositions", "driverChanges"})
			CHECK(node["counters"].contains(counter));
	const std::uint64_t cutBefore = number(snapshot["counters"], "cutCycles");

	// The stall, on purpose: the whole process stopped, then let go. The driver runs on the data thread it holds, so
	// every cycle the stall spans is cut in the catch-up's burst after it.
	process.signal(SIGSTOP);
	std::this_thread::sleep_for(stallFor); // the stall itself, made on purpose
	process.signal(SIGCONT);

	// One record carries the stall: most of its cycles, in one stall, not a record a cycle.
	const auto stall = process.waitForRecord(
		[](const json& record) { return isGraphRecord(record) && number(record, "cycles") >= stallCycles * 3 / 4; },
		deadline);
	INFO("ear printed:\n" << process.output());
	REQUIRE(stall);
	CHECK(number(*stall, "stalls") >= 1);
	CHECK(number(*stall, "ms") >= number(*stall, "cycles") * 256 * 1000 / 48000 - 1);
	const std::uint64_t total = number(*stall, "totalCycles");
	CHECK(total >= cutBefore + number(*stall, "cycles"));

	// The wire's sample reaches it, the graph's counters whole, lostMs the cut cycles' time.
	const json changed = rpc.waitFor([&](const json& message) {
		return message.value("method", "") == "counters.changed" &&
		       number(message["params"]["graph"], "cutCycles") >= total;
	});
	const json& graph = changed["params"]["graph"];
	CHECK(number(graph, "lostMs") == number(graph, "cutCycles") * 256 * 1000 / 48000);
	CHECK(number(graph, "stalls") >= number(*stall, "stalls"));
	CHECK(number(graph, "cycles") > number(snapshot["counters"], "cycles") + stallCycles);
	const json again = rpc.call("graph.get")["result"]["counters"];
	CHECK(number(again, "cutCycles") >= number(graph, "cutCycles"));

	// A second stall, and the stop asked for while ear is still stopped, so it begins as the catch-up's burst runs: the
	// record that burst makes is held back for the cuts to hold still, and the stop writes it all the same. Every
	// record's cycles then add up to the last one's total, the second stall's among them.
	process.signal(SIGSTOP);
	std::this_thread::sleep_for(stallFor); // the stall itself, made on purpose
	process.signal(SIGTERM);
	process.signal(SIGCONT);
	CHECK(rpc.readUntilClosed() == 1001);
	REQUIRE(waitForStop(process) == 0);
	const LostCycles lost = lostCycles(process);
	INFO(lost);
	std::uint64_t lastTotal = 0;
	for (const json& record : process.records())
		if (isGraphRecord(record)) lastTotal = number(record, "totalCycles");
	CHECK(lost.cutCycles == lastTotal);
	// How much of the second stall the burst cuts depends on how soon the stop removes the blocks; some of it does.
	CHECK(lost.cutCycles > number(again, "cutCycles"));
	CHECK(lost.graphRecords < lost.cutCycles); // never a record a cycle
}
