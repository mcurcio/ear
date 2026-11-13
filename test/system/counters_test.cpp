// The diagnostics counters reported end to end (docs/engine.md#reporting-the-counters): a stall of the real out/ear,
// made by stopping the whole process, reaches an ear.graph-lost-cycles record on standard output, the control port's
// snapshot, counters.changed and graph.get agree with the records, and at the stop the records add up to the total.
// How many stalls a host's stall is counted as is proven where it can be, in counter_watch_test.cpp and the engine's
// own tests, not here (docs/testing.md#the-counters-own-word). (Shared helpers: ear_system.hpp.)

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

#include "ear_system.hpp"

namespace {

using namespace ear::test;

/// How long the test holds ear stopped: every thread of it, the data thread among them, as a host that starves it does.
constexpr auto stallFor = 50ms;
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
/// moment the data thread is running (within 100 ms of looking); the catch-up's burst after it is cut on some hosts
/// and runs whole on others, a GitHub runner's among them (docs/engine.md#the-internal-clock).
void stall(ChildProcess& process, pid_t thread) {
	const auto until = std::chrono::steady_clock::now() + 100ms;
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

/// What a stall that cut cycles made: the cut cycles before it began, and once its catch-up ran.
struct Stalled {
	std::uint64_t before;
	std::uint64_t after;
};

/// Stalls ear until one stall cut a cycle, each judged by the graph's counters once it caught up: one that cut nothing
/// lost nothing, the host having run every cycle of it, and is made again.
Stalled stallUntilCut(ChildProcess& process, Rpc& rpc, pid_t thread) {
	const auto until = std::chrono::steady_clock::now() + 60s;
	while (std::chrono::steady_clock::now() < until) {
		const json before = rpc.call("graph.get")["result"]["counters"];
		const auto then = std::chrono::steady_clock::now();
		stall(process, thread);
		const std::uint64_t cut = number(caughtUp(rpc, number(before, "cycles"), then), "cutCycles");
		if (cut > number(before, "cutCycles")) return {number(before, "cutCycles"), cut};
	}
	FAIL("no stall cut a cycle within 60 s");
	return {};
}

/// Each graph record's total is the sum of every graph record's cycles up to it: the records' running totals agree.
void checkRunningTotals(const ChildProcess& process) {
	std::uint64_t sum = 0;
	for (const json& record : process.records()) {
		if (!isGraphRecord(record)) continue;
		sum += number(record, "cycles");
		INFO("a graph record: " << record.dump());
		CHECK(number(record, "totalCycles") == sum);
	}
}

} // namespace

TEST_CASE("A stall ear took reaches a graph record, the control port agrees with the records, and at the stop they add "
          "up to the total",
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

	// The stall: its cuts, counted by the graph, reach a record, which comes once they held still, within the pace.
	const Stalled stalled = stallUntilCut(process, rpc, *thread);
	INFO("the stall cut the count from " << stalled.before << " to " << stalled.after);
	const auto covering = process.waitForRecord(
		[&](const json& record) { return isGraphRecord(record) && number(record, "totalCycles") >= stalled.after; },
		deadline);
	INFO("ear printed:\n" << process.output());
	if (!covering) FAIL("no ear.graph-lost-cycles record covers the stall's cuts");
	INFO("the record that covers the stall: " << covering->dump());
	// The stall's cuts all come after it, held still at the beat that writes the record, so one record holds them all,
	// beside whatever else the host cut since the record before it.
	CHECK(number(*covering, "cycles") >= stalled.after - stalled.before);
	CHECK(number(*covering, "stalls") >= 1);
	CHECK(number(*covering, "ms") >= number(*covering, "cycles") * 256 * 1000 / 48000 - 1);
	checkRunningTotals(process);
	const std::uint64_t total = number(*covering, "totalCycles");

	// The wire's sample reaches it, the graph's counters whole, lostMs the cut cycles' time; graph.get after it agrees.
	const json changed = rpc.waitFor([&](const json& message) {
		return message.value("method", "") == "counters.changed" &&
		       number(message["params"]["graph"], "cutCycles") >= total;
	});
	const json& graph = changed["params"]["graph"];
	CHECK(number(graph, "lostMs") == number(graph, "cutCycles") * 256 * 1000 / 48000);
	CHECK(number(graph, "stalls") >= number(*covering, "stalls"));
	const json again = rpc.call("graph.get")["result"]["counters"];
	CHECK(number(again, "cutCycles") >= number(graph, "cutCycles"));
	CHECK(number(again, "lostMs") == number(again, "cutCycles") * 256 * 1000 / 48000);

	// A second stall, the stop asked for as soon as its cut is counted: its record may still be held back for the cuts
	// to hold still, and the stop writes it all the same. The records then add up to the last one's total, which is at
	// least every cut the control port counted.
	const Stalled second = stallUntilCut(process, rpc, *thread);
	process.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	REQUIRE(waitForStop(process) == 0);
	const LostCycles lost = lostCycles(process);
	INFO(lost);
	checkRunningTotals(process);
	std::uint64_t lastTotal = 0;
	for (const json& record : process.records())
		if (isGraphRecord(record)) lastTotal = number(record, "totalCycles");
	CHECK(lost.cutCycles == lastTotal);
	if (lost.cutCycles < second.after)
		FAIL("the records add up to " << lost.cutCycles << " cut cycles, short of the " << second.after
		                              << " the control port counted before the stop");
}
