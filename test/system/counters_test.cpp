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

/// What stalling until a cut made: the cut cycles once one stall cut some, or what intruded before the stall began.
struct Stalled {
	std::optional<std::uint64_t> cut;
	std::string intrusion;
};

/// Stalls ear until one stall cut a cycle, each judged by the graph's counters once it caught up: one that cut nothing
/// lost nothing, the host having run every cycle of it, and is made again. With `cutBefore`, a cut already past it when
/// a stall is about to begin is a host stall before the test's, returned as an intrusion.
Stalled stallUntilCut(ChildProcess& process, Rpc& rpc, pid_t thread, std::optional<std::uint64_t> cutBefore) {
	const auto until = std::chrono::steady_clock::now() + 60s;
	for (int attempts = 0; std::chrono::steady_clock::now() < until; ++attempts) {
		const json before = rpc.call("graph.get")["result"]["counters"];
		const auto then = std::chrono::steady_clock::now();
		if (cutBefore && number(before, "cutCycles") != *cutBefore)
			return {std::nullopt, "a host stall cut " + std::to_string(number(before, "cutCycles") - *cutBefore) +
			                          " cycles after the count, before the test's stall began"};
		stall(process, thread);
		const std::uint64_t cut = number(caughtUp(rpc, number(before, "cycles"), then), "cutCycles");
		if (cut > number(before, "cutCycles")) return {cut, {}};
	}
	FAIL("no stall cut a cycle within 60 s");
	return {};
}

/// The count of cut cycles a stall's record must start from: read once the graph has run 100 cycles (ear's first
/// cycles, while its blocks join, may cut one) and once every cut up to it is recorded, read again until it holds.
std::uint64_t recordedCount(ChildProcess& process, Rpc& rpc) {
	const auto until = std::chrono::steady_clock::now() + deadline;
	while (std::chrono::steady_clock::now() < until) {
		const json counters = rpc.call("graph.get")["result"]["counters"];
		if (number(counters, "cycles") < 100) continue;
		const std::uint64_t cut = number(counters, "cutCycles");
		if (cut > 0 &&
		    !process.waitForRecord(
				[&](const json& record) { return isGraphRecord(record) && number(record, "totalCycles") >= cut; },
				deadline))
			break;
		if (number(rpc.call("graph.get")["result"]["counters"], "cutCycles") == cut) return cut;
	}
	FAIL("the cut cycles were never all recorded and holding still");
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
	// The stall and its record. An attempt reads the count of cut cycles once every cut up to it is recorded, and stalls
	// ear until a stall cut a cycle. A host stall that lands before the test's stall begins (the count moved before the
	// stop, or its cuts have a record of their own before the stall's) says nothing of the stall's record, so the attempt
	// is made again, up to 5 times. One that lands inside the stall's record cannot be told from the stall and fails,
	// named: rerun alone (docs/testing.md#the-rules).
	constexpr std::size_t attemptsAllowed = 5;
	std::vector<std::string> intrusions;
	std::optional<json> stall;
	std::uint64_t cutBefore = 0;
	std::uint64_t cut = 0;
	while (!stall) {
		if (intrusions.size() == attemptsAllowed) {
			std::string all;
			for (const std::string& intrusion : intrusions) all += "\n  " + intrusion;
			FAIL("every one of " << attemptsAllowed << " attempts was intruded on by a host stall:" << all);
		}
		cutBefore = recordedCount(process, rpc);
		const Stalled made = stallUntilCut(process, rpc, *thread, cutBefore);
		if (!made.cut) {
			intrusions.push_back(made.intrusion);
			continue;
		}
		cut = *made.cut;
		// The first graph record of any cut since the count.
		auto first = process.waitForRecord(
			[&](const json& record) { return isGraphRecord(record) && number(record, "totalCycles") > cutBefore; },
			deadline);
		INFO("ear printed:\n" << process.output());
		REQUIRE(first);
		if (number(*first, "totalCycles") < cut) {
			// It ends before the stall's cuts: a host stall's own record, written between the count and the stop.
			intrusions.push_back("a host stall's record before the test's stall: " + first->dump());
			continue;
		}
		stall = std::move(first);
	}
	INFO("attempts: " << intrusions.size() + 1 << ", each intruded on before its stall:" << json(intrusions).dump());
	INFO("the first graph record after the count, which must be the stall's: " << stall->dump());
	// It starts at the count and carries one stall: every cycle the stall cut, not a record a cycle. Anything else is a
	// host stall inside the stall's record (it waits for the cuts to hold still), named here.
	if (number(*stall, "totalCycles") - number(*stall, "cycles") != cutBefore)
		FAIL("a host stall intruded on the stall's record: it starts at "
		     << number(*stall, "totalCycles") - number(*stall, "cycles") << " cut cycles, not the count's "
		     << cutBefore);
	if (number(*stall, "stalls") != 1)
		FAIL("a host stall intruded on the stall's record: it carries " << number(*stall, "stalls") << " stalls");
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
	const auto second = stallUntilCut(process, rpc, *thread, std::nullopt);
	REQUIRE(second.cut);
	const std::uint64_t secondCut = *second.cut;
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
