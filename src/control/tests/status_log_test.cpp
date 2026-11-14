// The one encoder of standard output (docs/output-records.md): every kind of record, byte for byte
// against test/protocol/output-records.json, and what it does with a full queue, bad text and many threads.
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "catch2/catch.hpp"
#include "ear/control/status_log.hpp"

using namespace ear::control;
using Json = nlohmann::json;

namespace {

/// Keeps every line offered, and refuses the ones it is told to (a full queue).
struct Lines final : OutputLines {
	std::vector<std::string> kept;
	int refuseNext = 0;
	bool offer(std::string line) override {
		if (refuseNext > 0) {
			--refuseNext;
			return false;
		}
		kept.push_back(std::move(line));
		return true;
	}
};

/// 2026-11-07T17:42:03.512094Z, and a few nanoseconds the record does not show.
std::chrono::system_clock::time_point fixedTime() {
	using namespace std::chrono;
	return sys_days{year{2026} / 11 / 7} + 17h + 42min + 3s + 512094us + 999ns;
}

/// The pace clock, moved by hand.
std::chrono::steady_clock::time_point paceNow{};
std::chrono::steady_clock::time_point pacedTime() { return paceNow; }

std::string fixtureText() {
	std::ifstream file(EAR_PROTOCOL_DIR "/output-records.json");
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

Json fixture() { return Json::parse(fixtureText()); }

/// Every key of each object a SAX parse captures, in document order (the vendored nlohmann/json sorts its own keys,
/// so this is how a test sees the order a record's fields are written in). With `recordsOnly`, one list per fixture
/// record (the object under each entry's "record"); otherwise one list for the whole text.
struct KeyOrder : nlohmann::json_sax<Json> {
	explicit KeyOrder(bool recordsOnly) : recordsOnly_(recordsOnly) {
		if (!recordsOnly) lists.emplace_back();
	}
	std::vector<std::vector<std::string>> lists;
	bool null() override { return true; }
	bool boolean(bool) override { return true; }
	bool number_integer(number_integer_t) override { return true; }
	bool number_unsigned(number_unsigned_t) override { return true; }
	bool number_float(number_float_t, const string_t&) override { return true; }
	bool string(string_t&) override { return true; }
	bool start_object(std::size_t) override {
		++depth_;
		if (recordsOnly_ && pendingRecord_) {
			pendingRecord_ = false;
			capturingFrom_ = depth_;
			lists.emplace_back();
		}
		return true;
	}
	bool key(string_t& key) override {
		if (!recordsOnly_ || capturingFrom_ > 0) lists.back().push_back(key);
		else if (depth_ == 2 && key == "record") pendingRecord_ = true;
		return true;
	}
	bool end_object() override {
		if (depth_ == capturingFrom_) capturingFrom_ = 0;
		--depth_;
		return true;
	}
	bool start_array(std::size_t) override { return true; }
	bool end_array() override { return true; }
	bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }

private:
	bool recordsOnly_;
	int depth_ = 0;
	int capturingFrom_ = 0;
	bool pendingRecord_ = false;
};

std::vector<std::vector<std::string>> keyOrders(const std::string& text, bool recordsOnly) {
	KeyOrder order(recordsOnly);
	REQUIRE(Json::sax_parse(text, &order));
	return order.lists;
}

/// A node's health in one phase, as NodeHost tells it.
ear::NodeHealth health(const std::string& node, ear::NodePhase phase, std::optional<ear::NodeCause> cause = {},
                       std::optional<ear::FailureReason> reason = {}, std::vector<ear::WaitingFor> waits = {}) {
	ear::NodeHealth told{node, ear::stateOf(phase), std::move(reason), phase};
	told.cause = std::move(cause);
	told.waitingFor = std::move(waits);
	return told;
}

ear::NodeCause cause(ear::NodeCause::Kind kind, std::vector<std::string> keys = {}) {
	return {kind, std::nullopt, keys};
}

ear::NodeCause restarting(ear::NodeCause::Kind kind, std::vector<std::string> keys = {}) {
	return {ear::NodeCause::Kind::restarting, kind, std::move(keys)};
}

GraphFile file(std::string version, std::optional<std::string> running) {
	return GraphFile{std::move(version), "text", {}, std::move(running), std::nullopt, {}, std::nullopt};
}

/// The fixture's records, made in its order.
std::vector<std::function<void(StatusLog&, Lines&)>> fixtureCalls() {
	using ear::FailureReason;
	using ear::NodePhase;
	using ear::RouteMatch;
	using Kind = ear::NodeCause::Kind;
	GraphFile everything = file("v9", std::nullopt);
	everything.diagnostics = {
		Diagnostic{Severity::error, "E2001", "unexpected token: \"}\"", TextPlace{9, 34}},
		Diagnostic{Severity::warning, "W3005", "nothing matched", TextPlace{10, 1}},
	};
	everything.refusal = Refusal{ear::UnknownPort{{"rec", "FX"}}, TextPlace{14, 1}};
	everything.pendingRestart = {FormatKey::graphFrequency, FormatKey::blockSize};
	everything.writeFailed = "ear.conf: No space left on device";
	const ear::Route toRec{{"noise", "noise.mono"}, {"rec", "record.left"}};
	const ear::Route toDesk{{"noise", "noise.mono"}, {"desk", "play.1"}};
	return {
		[](StatusLog& log, Lines&) { log.engineScheduling(true, "SCHED_FIFO 88"); },
		[](StatusLog& log, Lines&) { log.engineScheduling(false, "SCHED_OTHER"); },
		[](StatusLog& log, Lines&) { log.controlListening("127.0.0.1", 7777); },
		[](StatusLog& log, Lines&) { log.nodeChanged(health("desk", NodePhase::added)); },
		[](StatusLog& log, Lines&) {
			log.nodeChanged(
				health("desk", NodePhase::failed, {}, FailureReason{FailureReason::Code::engineRefused, "card lost"}));
		},
		[](StatusLog& log, Lines&) { log.nodeGone("rec", 0); },
		[](StatusLog& log, Lines&) { log.nodeSays("kitchen", "connected \"Music Assistant\" unpaired"); },
		[toRec](StatusLog& log, Lines&) { log.routeChanged({toRec, RouteMatch::linked, ""}); },
		[toDesk](StatusLog& log, Lines&) { log.routeChanged({toDesk, RouteMatch::refused, "link failed"}); },
		[](StatusLog& log, Lines&) { log.graphFileChanged({file("v2", "v2"), FileChangeCause::edit, 0}); },
		[everything](StatusLog& log, Lines&) { log.graphFileChanged({everything, FileChangeCause::disk, 2}); },
		[](StatusLog& log, Lines&) { log.graphFileWritten("v7"); },
		[](StatusLog& log, Lines&) { log.graphFileWriteFailed("ear.conf: No space left on device"); },
		[](StatusLog& log, Lines&) {
			log.log({.level = ear::LogLevel::warn,
		             .source = "stderr",
		             .text = "W sendspin.client: \x1b[1mtime\x1b[0m skew"});
		},
		[](StatusLog& log, Lines&) {
			log.log({.level = ear::LogLevel::error,
		             .source = "pipewire",
		             .text = "'hw:Pro24': playback open failed: No such file or directory",
		             .topic = "spa.alsa"});
		},
		[](StatusLog& log, Lines&) {
			log.log({.level = ear::LogLevel::info,
		             .source = "sendspin",
		             .text = "key file narrowed",
		             .node = "kitchen",
		             .cut = true});
		},
		[](StatusLog& log, Lines&) { log.exiting(1, "control port: cannot bind 0.0.0.0:7777: Address in use"); },
		[](StatusLog& log, Lines& lines) {
			lines.refuseNext = 1;
			log.nodeChanged(health("desk", NodePhase::stopping, cause(Kind::stop))); // dropped: seq 18 never appears
			log.nodeChanged(health("desk", NodePhase::running));
		},
		[](StatusLog& log, Lines& lines) {
			// A flood from one source: StatusLog::logBurst pass (seq 20 to 51, not the fixture's, so set aside), the
			// rest are held back, and a second later the next passes and counts them.
			const std::size_t before = lines.kept.size();
			for (int i = 0; i < StatusLog::logBurst + 9; ++i)
				log.log({.level = ear::LogLevel::error, .source = "alsa-lib", .text = "flood"});
			REQUIRE(lines.kept.size() == before + StatusLog::logBurst);
			lines.kept.resize(before);
			paceNow += std::chrono::seconds(1);
			log.log({.level = ear::LogLevel::error,
		             .source = "alsa-lib",
		             .text = "pcm.c:2664:(snd_pcm_open_noupdate) Unknown PCM hw:Pro24"});
		},
		[](StatusLog& log, Lines&) {
			log.nodeDiagnostic("kitchen", "ear.close-overdue", "its run did not close within 10 s, so it was leaked",
		                       {});
		},
		[](StatusLog& log, Lines&) {
			const std::array fields{ear::DiagnosticField{"ms", 100}};
			log.nodeDiagnostic("desk", "ear.fade-unacknowledged",
		                       "the fade before its silence was not acknowledged within 100 ms, so the silence was "
		                       "taken unfaded",
		                       fields);
		},
		[](StatusLog& log, Lines&) {
			const std::array fields{ear::DiagnosticField{"ms", 5000}};
			log.nodeDiagnostic("desk", "ear.barrier-overdue",
		                       "a barrier posted to the data loop did not complete within 5000 ms, so its "
		                       "configuration and block are leaked",
		                       fields);
		},
		[](StatusLog& log, Lines&) {
			log.engineDiagnostic({"ear.data-loop-wedged",
		                          "node desk's barrier did not complete within 5000 ms: the data loop runs no invoke, "
		                          "so no further engine change is applied",
		                          {ear::DiagnosticField{"ms", 5000}}});
		},
		[](StatusLog& log, Lines&) { log.mainLoopStalled(612, "pw_filter_destroy desk"); },
		[](StatusLog& log, Lines&) { log.mainLoopStalled(503, ""); },
		[](StatusLog& log, Lines&) { log.mainLoopResumed(1840); },
		// The lifecycle's records (docs/output-records.md#node), one per row 3a has and per cause.kind: a record when
		// the state changes, `starting: <cause>` for configuring after a restart, a retry or an enable, and none for
		// the colour's other phases (configured, starting) or a change of the other members alone.
		[](StatusLog& log, Lines&) {
			log.nodeChanged(health("porch", NodePhase::configuring)); // no record: configuring with no cause
			log.nodeChanged(health("porch", NodePhase::configured));
			log.nodeChanged(health("porch", NodePhase::starting));
			log.nodeChanged(health("porch", NodePhase::running));
		},
		[](StatusLog& log, Lines&) { log.nodeChanged(health("porch", NodePhase::stopping, cause(Kind::stop))); },
		[](StatusLog& log, Lines&) { log.nodeChanged(health("porch", NodePhase::stopped)); },
		[](StatusLog& log, Lines&) { log.nodeChanged(health("porch", NodePhase::stopping, cause(Kind::disabling))); },
		[](StatusLog& log, Lines&) { log.nodeChanged(health("porch", NodePhase::disabled)); },
		[](StatusLog& log, Lines&) { log.nodeChanged(health("porch", NodePhase::configuring, cause(Kind::enabling))); },
		[](StatusLog& log, Lines&) {
			log.nodeChanged(health("rec", NodePhase::stopping, restarting(Kind::settings, {"path"})));
		},
		[](StatusLog& log, Lines&) {
			log.nodeChanged(health("rec", NodePhase::configuring, cause(Kind::settings, {"path"})));
		},
		[](StatusLog& log, Lines&) {
			log.nodeChanged(health("rec", NodePhase::starting));
			log.nodeChanged(health("rec", NodePhase::running));
		},
		[](StatusLog& log, Lines&) { log.nodeChanged(health("rec", NodePhase::stopping, restarting(Kind::action))); },
		[](StatusLog& log, Lines&) { log.nodeChanged(health("rec", NodePhase::configuring, cause(Kind::action))); },
		[](StatusLog& log, Lines&) {
			log.nodeChanged(
				health("kitchen", NodePhase::failed, {}, FailureReason{FailureReason::Code::lost, "the library died"}));
		},
		[](StatusLog& log, Lines&) { log.nodeChanged(health("kitchen", NodePhase::configuring, cause(Kind::retry))); },
		[](StatusLog& log, Lines&) { log.nodeChanged(health("kitchen", NodePhase::stopping, cause(Kind::removing))); },
		[](StatusLog& log, Lines&) {
			// Its phase began 2 s ago: NodeHost adds what it waits for only then, and the record says how long.
			log.nodeChanged(health("fpp", NodePhase::configuring));
			paceNow += std::chrono::seconds(2);
			log.nodeChanged(
				health("fpp", NodePhase::configuring, {}, {}, {{ear::WaitingFor::Kind::companion, "fpp.rtp"}}));
		},
		[](StatusLog& log, Lines&) {
			// A change within 10 s of the last still record writes none; the first tell after the pace writes the newest
			// list if it differs from the one written, though the list itself did not change again.
			const std::vector<ear::WaitingFor> newest{{ear::WaitingFor::Kind::olderNode, "fpp"},
		                                              {ear::WaitingFor::Kind::consumer}};
			log.nodeChanged(health("fpp", NodePhase::configuring, {}, {},
		                           {{ear::WaitingFor::Kind::block}, {ear::WaitingFor::Kind::companion, "fpp.rtp"}}));
			log.nodeChanged(health("fpp", NodePhase::configuring, {}, {}, newest));
			paceNow += std::chrono::seconds(10);
			ear::NodeHealth again = health("fpp", NodePhase::configuring, {}, {}, newest);
			again.wanted = ear::Wanted::stopped; // another member changed: a tell with the same list
			log.nodeChanged(again);
		},
		[](StatusLog& log, Lines&) {
			const std::array fields{ear::DiagnosticField{"ms", 31}};
			log.nodeDiagnostic("rec", "ear.hook-slow", "configure ran 31 ms", fields);
		},
		[](StatusLog& log, Lines&) { log.actionAnswered({"desk", "restart", "ok", std::nullopt}); },
		[](StatusLog& log, Lines&) { log.actionAnswered({"desk", "restart", "not-restartable", "block-lost"}); },
		[](StatusLog& log, Lines&) { log.actionAnswered({"porch", "start", "node-disabled", std::nullopt}); },
		[](StatusLog& log, Lines&) {
			log.internalError(
				{.callback = "FilterPortBlock::onStateChanged", .node = "hall", .where = {}, .what = "bad state"});
		},
		[](StatusLog& log, Lines&) {
			log.internalError({.callback = "MainLoopExecutor's posted work",
		                       .node = {},
		                       .where = "make block hall",
		                       .what = "a block made twice",
		                       .fatal = true});
		},
		[](StatusLog& log, Lines&) {
			log.internalError({.callback = "LinkProxy::onInfo", .node = {}, .where = {}, .what = "bad info"});
		},
		[](StatusLog& log, Lines&) {
			log.graphLostCycles({.cycles = 18750, .cutCycles = 39, .stalls = 1},
		                        {.cycles = 900, .cutCycles = 39, .stalls = 1}, 208);
		},
		[](StatusLog& log, Lines&) {
			log.nodeMissedCycles("rec", {.missedCycles = 33, .overruns = 2, .repositions = 1},
		                         {.missedCycles = 33, .overruns = 1, .repositions = 1});
		},
		[](StatusLog& log, Lines&) {
			log.companionFramesLost("rec",
		                            {.name = "rec.writer",
		                             .kind = ear::CompanionKind::sink,
		                             .bank = "writer",
		                             .ready = true,
		                             .dropped = 8192},
		                            4096, 8192);
		},
		[](StatusLog& log, Lines&) {
			log.companionFramesLost("kitchen",
		                            {.name = "kitchen.from_server",
		                             .kind = ear::CompanionKind::source,
		                             .bank = "from_server",
		                             .ready = true,
		                             .shortfall = 256},
		                            1, 256);
		},
		[](StatusLog& log, Lines&) {
			const std::array fields{ear::DiagnosticField{"ms", 140}};
			log.nodeDiagnostic("porch", "ear.loop-slow", "loop control ran one handler for 140 ms", fields);
		},
		[](StatusLog& log, Lines&) {
			const std::array fields{ear::DiagnosticField{"sources", 2}};
			log.nodeDiagnostic("kitchen", "ear.loop-source-left",
		                       "2 sources were still armed on loop sendspin when the node's configuration closed",
		                       fields);
		},
		[](StatusLog& log, Lines&) {
			// fpp is still configuring (above); 10 s after its last still record, a new list is written.
			paceNow += std::chrono::seconds(10);
			log.nodeChanged(health("fpp", NodePhase::configuring, {}, {}, {{ear::WaitingFor::Kind::loop, "sendspin"}}));
		},
		[](StatusLog& log, Lines&) {
			// Still configuring, 10 s on: now it waits for one of its stores to open, by its file's name.
			paceNow += std::chrono::seconds(10);
			log.nodeChanged(
				health("fpp", NodePhase::configuring, {}, {}, {{ear::WaitingFor::Kind::store, "sendspin-fpp.key"}}));
		},
		[](StatusLog& log, Lines&) {
			const std::array fields{ear::DiagnosticField{"ms", 14250}};
			log.nodeDiagnostic("kitchen", "ear.close-late",
		                       "the configuration leaked at the close deadline closed 14250 ms after its close began",
		                       fields);
		},
		[](StatusLog& log, Lines&) {
			// Still configuring, 10 s on: now it waits for one of its claims, whose holder is closing, by its key.
			paceNow += std::chrono::seconds(10);
			log.nodeChanged(
				health("fpp", NodePhase::configuring, {}, {}, {{ear::WaitingFor::Kind::claim, "udp:5004"}}));
		},
		[](StatusLog& log, Lines&) {
			log.nodeDiagnostic("fpp2", "ear.claim-wait", "udp:5004 held by node fpp, which is closing", {});
		},
		[](StatusLog& log, Lines&) {
			// Still configuring, 10 s on: now it waits for one of its recordings to open, by its path.
			paceNow += std::chrono::seconds(10);
			log.nodeChanged(
				health("fpp", NodePhase::configuring, {}, {}, {{ear::WaitingFor::Kind::recording, "take1.wav"}}));
		},
		[](StatusLog& log, Lines&) { log.stopped(); },
	};
}

} // namespace

TEST_CASE("Every kind of record is the fixture's, field for field and in its order", "[status_log]") {
	const Json records = fixture()["records"];
	const auto orders = keyOrders(fixtureText(), true);
	const auto calls = fixtureCalls();
	REQUIRE(records.size() == calls.size());
	REQUIRE(orders.size() == calls.size());
	Lines lines;
	StatusLog log(lines, &fixedTime, &pacedTime);
	for (std::size_t i = 0; i < calls.size(); ++i) {
		INFO(records[i]["case"].get<std::string>());
		calls[i](log, lines);
		REQUIRE(lines.kept.size() == i + 1);
		CHECK(Json::parse(lines.kept[i]) == records[i]["record"]);
		CHECK(keyOrders(lines.kept[i], false).front() == orders[i]);
	}
}

TEST_CASE("Every kind the fixture shows is made, and each record is one line", "[status_log]") {
	std::set<std::string> kinds;
	const Json records = fixture()["records"];
	for (const Json& entry : records) kinds.insert(entry["record"]["kind"].get<std::string>());
	CHECK(kinds == std::set<std::string>{"engine", "control", "node", "note", "route", "graph-file",
	                                     "graph-file-written", "graph-file-write-failed", "log", "exit", "diagnostic",
	                                     "action"});
	Lines lines;
	StatusLog log(lines, &fixedTime, &pacedTime);
	for (const auto& call : fixtureCalls()) call(log, lines);
	for (const std::string& line : lines.kept) CHECK(line.find('\n') == std::string::npos);
}

TEST_CASE("Text that is no UTF-8 is replaced, never thrown or passed through", "[status_log]") {
	Lines lines;
	StatusLog log(lines, &fixedTime);
	log.nodeSays("a", std::string("bad \xff\xfe byte"));
	REQUIRE(lines.kept.size() == 1);
	const Json record = Json::parse(lines.kept[0]);
	CHECK(record["text"] == "bad \xEF\xBF\xBD\xEF\xBF\xBD byte");
}

TEST_CASE("Drops are counted until a record gets through, and only that record says so", "[status_log]") {
	Lines lines;
	StatusLog log(lines, &fixedTime);
	lines.refuseNext = 3;
	for (int i = 0; i < 4; ++i) log.graphFileWritten("v" + std::to_string(i));
	log.graphFileWritten("v4");
	REQUIRE(lines.kept.size() == 2);
	const Json first = Json::parse(lines.kept[0]);
	CHECK(first["seq"] == 4);
	CHECK(first["dropped"] == 3);
	CHECK(first["msg"] == "graph-file written v3 (3 records dropped before this)");
	const Json second = Json::parse(lines.kept[1]);
	CHECK(second["seq"] == 5);
	CHECK_FALSE(second.contains("dropped"));
}

TEST_CASE("Records from many threads keep one order: seq rises by one a line", "[status_log]") {
	Lines lines;
	StatusLog log(lines);
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t)
		threads.emplace_back([&log, t] {
			for (int i = 0; i < 250; ++i) log.nodeSays("n" + std::to_string(t), std::to_string(i));
		});
	for (std::thread& thread : threads) thread.join();
	REQUIRE(lines.kept.size() == 1000);
	for (std::size_t i = 0; i < lines.kept.size(); ++i) CHECK(Json::parse(lines.kept[i])["seq"] == i + 1);
}

TEST_CASE("The watchdog's stall records, from its own thread, keep one order with the main thread's", "[status_log]") {
	Lines lines;
	StatusLog log(lines);
	std::thread watchdog([&log] {
		for (int i = 0; i < 200; ++i) {
			log.mainLoopStalled(500 + i, "configure kitchen");
			log.mainLoopResumed(600 + i);
		}
	});
	for (int i = 0; i < 400; ++i) log.nodeSays("kitchen", std::to_string(i));
	watchdog.join();
	REQUIRE(lines.kept.size() == 800);
	std::int64_t nextStall = 500;
	std::int64_t nextResume = 600;
	bool stalled = false;
	for (std::size_t i = 0; i < lines.kept.size(); ++i) {
		const Json record = Json::parse(lines.kept[i]);
		CHECK(record["seq"] == i + 1);
		if (record["kind"] != "diagnostic") continue;
		if (record["code"] == "ear.main-loop-stalled") {
			CHECK_FALSE(stalled);
			CHECK(record["ms"] == nextStall++);
			stalled = true;
		} else {
			CHECK(stalled);
			CHECK(record["ms"] == nextResume++);
			stalled = false;
		}
	}
	CHECK(nextStall == 700);
}

TEST_CASE("A source that floods its log is paced: node states, routes and notes still get through", "[status_log]") {
	/// A queue of 100 lines that nobody reads while the flood lasts.
	struct Bounded final : OutputLines {
		std::vector<std::string> kept;
		bool offer(std::string line) override {
			if (kept.size() >= 100) return false;
			kept.push_back(std::move(line));
			return true;
		}
	} lines;
	StatusLog log(lines, &fixedTime, &pacedTime);
	for (int i = 0; i < 10000; ++i) {
		log.log({.level = ear::LogLevel::warn, .source = "stderr", .text = "E sendspin.client: chatter"});
		log.log({.level = ear::LogLevel::error, .source = "pipewire", .text = "xrun", .topic = "spa.driver"});
	}
	log.log({.level = ear::LogLevel::info, .source = "sendspin", .text = "a node's own", .node = "kitchen"});
	log.nodeSays("kitchen", "pairing-code 123-456");
	log.nodeChanged({"desk", ear::NodeState::running, std::nullopt, ear::NodePhase::running});
	REQUIRE(lines.kept.size() == 2 * StatusLog::logBurst + 3);
	CHECK(Json::parse(lines.kept[lines.kept.size() - 3])["node"] == "kitchen");
	const Json note = Json::parse(lines.kept[lines.kept.size() - 2]);
	CHECK(note["kind"] == "note");
	CHECK(note["text"] == "pairing-code 123-456");
	CHECK_FALSE(note.contains("dropped"));
	CHECK(Json::parse(lines.kept.back())["state"] == "running");

	// A second on, each source passes one more, counting what it held back.
	paceNow += std::chrono::seconds(1);
	lines.kept.clear();
	log.log({.level = ear::LogLevel::warn, .source = "stderr", .text = "still here"});
	log.log({.level = ear::LogLevel::warn, .source = "stderr", .text = "held"});
	REQUIRE(lines.kept.size() == 1);
	const Json counted = Json::parse(lines.kept[0]);
	CHECK(counted["heldBack"] == 10000 - StatusLog::logBurst);
	CHECK(counted["msg"] == "stderr warn: still here (9968 more held back before this: over 32 at once)");
}

TEST_CASE("A superseded node's records are kept apart from the newer node of its name", "[status_log]") {
	using ear::NodePhase;
	Lines lines;
	StatusLog log(lines, &fixedTime, &pacedTime);
	const auto removing = cause(ear::NodeCause::Kind::removing);
	log.nodeChanged(health("x", NodePhase::running));
	ear::NodeHealth old = health("x", NodePhase::stopping, removing);
	old.superseded = true;
	log.nodeChanged(old);                                 // the older node stops (another type replaced it)
	log.nodeChanged(health("x", NodePhase::added));       // the newer node
	log.nodeChanged(old);                                 // the older node told again: nothing new
	log.supersededGone("x", 3);                           // its removed, leaving the newer node's state alone
	log.nodeChanged(health("x", NodePhase::configuring)); // still starting: no record
	log.nodeChanged(health("x", NodePhase::running));
	std::vector<std::string> msgs;
	for (const std::string& line : lines.kept) msgs.push_back(Json::parse(line)["msg"]);
	CHECK(msgs == std::vector<std::string>{"node x running", "node x stopping: removing", "node x starting",
	                                       "node x removed dropped=3", "node x running"});
}
