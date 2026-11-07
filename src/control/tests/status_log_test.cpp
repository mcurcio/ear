// The one encoder of standard output (docs/design/node-framework.md section 8.8): every kind of record, byte for byte
// against test/protocol/output-records.json, and what it does with a full queue, bad text and many threads.
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

GraphFile file(std::string version, std::optional<std::string> running) {
	return GraphFile{std::move(version), "text", {}, std::move(running), std::nullopt, {}, std::nullopt};
}

/// The fixture's records, made in its order.
std::vector<std::function<void(StatusLog&, Lines&)>> fixtureCalls() {
	using ear::FailureReason;
	using ear::NodeState;
	using ear::RouteMatch;
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
		[](StatusLog& log, Lines&) { log.nodeChanged({"desk", NodeState::starting, std::nullopt}); },
		[](StatusLog& log, Lines&) {
			log.nodeChanged(
				{"desk", NodeState::failed, FailureReason{FailureReason::Code::engineRefused, "card lost"}});
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
		             .run = 2,
		             .cut = true});
		},
		[](StatusLog& log, Lines&) { log.exiting(1, "control port: cannot bind 0.0.0.0:7777: Address in use"); },
		[](StatusLog& log, Lines& lines) {
			lines.refuseNext = 1;
			log.nodeChanged({"desk", NodeState::stopping, std::nullopt}); // dropped: seq 18 never appears
			log.nodeChanged({"desk", NodeState::running, std::nullopt});
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
			log.nodeDiagnostic("kitchen", "ear.close-overdue", "its run did not close within 10 s, so it was leaked");
		},
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
	                                     "graph-file-written", "graph-file-write-failed", "log", "exit"});
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
	log.nodeChanged({"desk", ear::NodeState::running, std::nullopt});
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
