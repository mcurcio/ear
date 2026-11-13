// The diagnostics counters reported end to end (docs/engine.md#reporting-the-counters): a stall of the real out/ear,
// made by stopping the whole process, is one ear.graph-lost-cycles record on standard output however many cycles it
// cut, and the control port's snapshot and counters.changed carry the same counts. (Shared helpers: ear_system.hpp.)

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

#include <sys/types.h>

#include "ear_system.hpp"

namespace {

using namespace ear::test;

/// How long the test holds ear stopped: every thread of it, the data thread among them, as a host that starves it does.
constexpr auto stallFor = 200ms;
/// A cycle at the graph's 48 kHz and block of 256.
constexpr auto cycleTime = std::chrono::microseconds(5333);

bool isGraphRecord(const json& record) { return record.value("code", "") == "ear.graph-lost-cycles"; }

std::uint64_t number(const json& object, const char* key) { return object.at(key).get<std::uint64_t>(); }

/// ear's data thread, by the name PipeWire gives it.
std::optional<pid_t> dataThread(pid_t process) {
	for (const auto& task : std::filesystem::directory_iterator("/proc/" + std::to_string(process) + "/task")) {
		std::ifstream comm(task.path() / "comm");
		std::string name;
		std::getline(comm, name);
		if (name == "data-loop.0") return static_cast<pid_t>(std::stol(task.path().filename().string()));
	}
	return std::nullopt;
}

/// Whether the thread is running now: state R in its stat, after the name in parentheses.
bool running(pid_t process, pid_t thread) {
	std::ifstream stat("/proc/" + std::to_string(process) + "/task/" + std::to_string(thread) + "/stat");
	std::string text((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
	const auto close = text.rfind(')');
	return close != std::string::npos && close + 2 < text.size() && text[close + 2] == 'R';
}

/// One stall, made on purpose: the whole of ear stopped, then let go. Which cycles it cuts is the host's: the cut that
/// is certain on any host is of a cycle held in progress past the driver's next timeout, so the stop is aimed at a
/// moment the data thread is running (within a second of looking); the catch-up's burst after it is cut on some hosts
/// and runs whole on others, a GitHub runner's among them (docs/engine.md#the-internal-clock).
void stall(ChildProcess& process, pid_t thread) {
	const auto until = std::chrono::steady_clock::now() + 1s;
	while (!running(process.pid(), thread) && std::chrono::steady_clock::now() < until) {
	}
	process.signal(SIGSTOP);
	std::this_thread::sleep_for(stallFor); // the stall itself, made on purpose
	process.signal(SIGCONT);
}

/// The graph's counters once its cycles caught up with the time since `then`, when it had started `cyclesThen`: the
/// catch-up's burst after a stall has run, so every cut it made is counted. Each graph.get is the look that decides.
json caughtUp(Rpc& rpc, std::uint64_t cyclesThen, std::chrono::steady_clock::time_point then) {
	const auto until = std::chrono::steady_clock::now() + deadline;
	while (std::chrono::steady_clock::now() < until) {
		const auto owed = static_cast<std::uint64_t>((std::chrono::steady_clock::now() - then) / cycleTime);
		json counters = rpc.call("graph.get")["result"]["counters"];
		if (number(counters, "cycles") + 2 >= cyclesThen + owed) return counters;
	}
	FAIL("the graph's cycles never caught up after a stall");
	return {};
}

/// Stalls ear until one stall cut a cycle, each judged by the graph's counters once it caught up: one that cut nothing
/// lost nothing, the host having run every cycle of it, and is made again. With `cutBefore`, no cut but the test's
/// may come between (a host stall fails it, named). Returns the cut cycles then.
std::uint64_t stallUntilCut(ChildProcess& process, Rpc& rpc, pid_t thread, std::optional<std::uint64_t> cutBefore) {
	const auto until = std::chrono::steady_clock::now() + 60s;
	for (int attempts = 0; std::chrono::steady_clock::now() < until; ++attempts) {
		const json before = rpc.call("graph.get")["result"]["counters"];
		const auto then = std::chrono::steady_clock::now();
		INFO("attempt " << attempts << ", before: " << before.dump());
		// A host stall between the count and the test's: the host's cuts, not the test's, so rerun alone.
		if (cutBefore && number(before, "cutCycles") != *cutBefore)
			FAIL("a host stall intruded before the test's: " << number(before, "cutCycles") - *cutBefore
			                                                 << " cycles cut since the count, " << before.dump());
		stall(process, thread);
		const std::uint64_t cut = number(caughtUp(rpc, number(before, "cycles"), then), "cutCycles");
		if (cut > number(before, "cutCycles")) return cut;
	}
	FAIL("no stall cut a cycle within 60 s");
	return 0;
}

} // namespace

TEST_CASE("A stall is one ear.graph-lost-cycles record however many cycles it cut, and the control port carries its "
          "counts",
          "[system][counters]") {
	TempDirectory directory;
	writeGraphFile(directory, noiseToRec());
	Ear ear(directory, {linkedLine("noise:noise.mono", "rec:record.left")});
	ChildProcess& process = ear.process();
	Rpc rpc(ear.port());
	const auto thread = dataThread(process.pid());
	REQUIRE(thread);

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
	// The count the stall's record must start from, read once the graph has run 100 cycles: ear's first cycles, while
	// its blocks join, may cut one (a snapshot taken at once can read 1 cycle and no cut), and the cuts before the count
	// are recorded before the stall, so its record carries the test's stall alone.
	json baseline = snapshot["counters"];
	while (number(baseline, "cycles") < 100) baseline = rpc.call("graph.get")["result"]["counters"];
	const std::uint64_t cutBefore = number(baseline, "cutCycles");
	if (cutBefore > 0) {
		INFO("ear printed:\n" << process.output());
		REQUIRE(process.waitForRecord(
			[&](const json& record) { return isGraphRecord(record) && number(record, "totalCycles") >= cutBefore; },
			deadline));
	}

	const std::uint64_t cut = stallUntilCut(process, rpc, *thread, cutBefore);

	// One record carries the stall, and it is the first graph record of any cut since the count: every cycle the stall
	// cut from the count on, in one stall, not a record a cycle. A host stall between the count and the test's shows
	// here as an earlier record, or as a second stall in this one: rerun alone (docs/testing.md#the-rules).
	const auto stall = process.waitForRecord(
		[&](const json& record) { return isGraphRecord(record) && number(record, "totalCycles") > cutBefore; },
		deadline);
	INFO("ear printed:\n" << process.output());
	REQUIRE(stall);
	INFO("the first graph record after the count, which must be the stall's: " << stall->dump());
	// Its record starts at the count and carries one stall; any other is a host stall that came between the count and
	// the record (the record waits for the cuts to hold still), named here: rerun alone.
	if (number(*stall, "totalCycles") - number(*stall, "cycles") != cutBefore)
		FAIL("a host stall intruded: the stall's record starts at "
		     << number(*stall, "totalCycles") - number(*stall, "cycles") << " cut cycles, not the count's "
		     << cutBefore);
	if (number(*stall, "stalls") != 1)
		FAIL("a host stall intruded: the stall's record carries " << number(*stall, "stalls") << " stalls");
	CHECK(number(*stall, "totalCycles") >= cut);
	CHECK(number(*stall, "ms") >= number(*stall, "cycles") * 256 * 1000 / 48000 - 1);
	const std::uint64_t total = number(*stall, "totalCycles");

	// The wire's sample reaches it, the graph's counters whole, lostMs the cut cycles' time.
	const json changed = rpc.waitFor([&](const json& message) {
		return message.value("method", "") == "counters.changed" &&
		       number(message["params"]["graph"], "cutCycles") >= total;
	});
	const json& graph = changed["params"]["graph"];
	CHECK(number(graph, "lostMs") == number(graph, "cutCycles") * 256 * 1000 / 48000);
	CHECK(number(graph, "stalls") >= number(*stall, "stalls"));
	const json again = rpc.call("graph.get")["result"]["counters"];
	CHECK(number(again, "cutCycles") >= number(graph, "cutCycles"));

	// A second stall, the stop asked for as soon as its cut is counted: the record of it may still be held back for the
	// cuts to hold still, and the stop writes it all the same. Every record's cycles then add up to the last one's total,
	// the second stall's among them.
	const std::uint64_t secondCut = stallUntilCut(process, rpc, *thread, std::nullopt);
	process.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	REQUIRE(waitForStop(process) == 0);
	const LostCycles lost = lostCycles(process);
	INFO(lost);
	std::uint64_t lastTotal = 0;
	for (const json& record : process.records())
		if (isGraphRecord(record)) lastTotal = number(record, "totalCycles");
	CHECK(lost.cutCycles == lastTotal);
	CHECK(lost.cutCycles >= secondCut);
	CHECK(lost.graphRecords <= lost.stalls); // a record a stall at most, never a record a cycle
}
