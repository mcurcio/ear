// The graph file keeper's sequencer (keeper.hpp; docs/architecture.md#how-a-change-moves), driven on the test thread
// with two executors the test pumps: the main loop and the keeper's own. A real DesiredGraph over the test types
// judges; ear.conf is a real file in a temp folder (FolderGraphFileDisk), so both collisions come from a hand edit made
// between explicit steps. One fake disk gives the write failures a real folder cannot give cheaply.
#include <algorithm>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "catch2/catch.hpp"
#include "keeper.hpp"
#include "keeper_support.hpp"
#include "test_types.hpp"

using namespace ear;
using namespace ear::config;
using namespace ear::config::test;
using control::CommandResult;
using control::FileChangeCause;
using json = nlohmann::json;

namespace {

/// Posted work waits until the test runs it. The main loop's counts its turns into the log.
class QueueExecutor final : public Executor {
public:
	explicit QueueExecutor(Log* log = nullptr) : log_(log) {}
	void post(std::move_only_function<void()> work) override { queue_.push_back(std::move(work)); }
	bool empty() const { return queue_.empty(); }
	void runOne() {
		REQUIRE_FALSE(queue_.empty());
		auto work = std::move(queue_.front());
		queue_.pop_front();
		if (log_ != nullptr) ++log_->turn;
		work();
	}

private:
	Log* log_;
	std::deque<std::move_only_function<void()>> queue_;
};

/// The graph read: the desired graph's nodes, and a log line for each read.
class Reader final : public GraphReader {
public:
	Reader(const DesiredGraph& graph, Log& log) : graph_(graph), log_(log) {}
	GraphView read() const override {
		log_.push("read graph");
		GraphView view{{{ear::SampleRate::fixed<48000>(), 256}, false, "SCHED_OTHER"}, {}, {}, {}};
		for (auto& node : graph_.nodes()) view.nodes.push_back({node.spec, node.ports, healthFromSpec(node.spec)});
		return view;
	}

private:
	const DesiredGraph& graph_;
	Log& log_;
};

/// ear.conf in memory, with a switch that makes every replace fail.
class FakeDisk final : public GraphFileDisk {
public:
	explicit FakeDisk(std::optional<std::string> text) : text(std::move(text)) {}
	std::optional<std::string> text;
	bool failing = false;
	std::string failure = "disk full";
	int replaces = 0;
	DiskText read() override { return text; }
	std::expected<folder::ReplaceOutcome, std::string> replaceIf(const std::optional<std::string>& expected,
	                                                             std::string_view next) override {
		++replaces;
		if (failing) return std::unexpected(failure);
		if (expected != text) return folder::ReplaceOutcome{folder::Changed{text}};
		text = std::string(next);
		return folder::ReplaceOutcome{folder::Replaced{}};
	}
};

folder::ConfinedFolder openFolder(const std::filesystem::path& path) {
	auto folder = folder::ConfinedFolder::open(path);
	REQUIRE(folder.has_value());
	return std::move(*folder);
}

struct Fixture {
	/// ear.conf holds `boot` (nothing: missing). `fake`: the disk to use instead of the folder.
	explicit Fixture(const std::string& name, std::optional<std::string> boot = recordExample, FakeDisk* fake = nullptr,
	                 AudioFormat engine = {ear::SampleRate::fixed<48000>(), 256})
		: dir(name), file(dir.path() / "ear.conf"), folder(openFolder(dir.path())) {
		if (boot) std::ofstream(file, std::ios::binary) << *boot;
		graph.subscribe(static_cast<DesiredNodeListener&>(graphLog));
		graph.subscribe(static_cast<DesiredRouteListener&>(graphLog));
		side.subscribe(fileLog);
		side.subscribeActions(actions);
		side.answerActions(nodeActions);
		GraphFileDisk& disk = fake != nullptr ? static_cast<GraphFileDisk&>(*fake) : realDisk;
		DiskText base = disk.read();
		auto first = GraphFileModel::load(base, catalogue);
		keeper.emplace(std::move(base), std::move(first), disk, engine, side, main, self);
	}

	/// Runs every posted item, the main loop's and the keeper's, until none is left.
	void pump() {
		while (!main.empty() || !self.empty()) {
			while (!main.empty()) main.runOne();
			while (!self.empty()) self.runOne();
		}
	}
	void start() {
		keeper->start();
		pump();
		log.clear();
	}
	void free() {
		keeper->free();
		pump();
	}
	void diskEvent() {
		keeper->diskEvent();
		pump();
	}
	/// Submits a batch without running anything; `got` holds the reply once it comes.
	void request(std::vector<control::Command> commands, std::optional<std::vector<CommandResult>>& got) {
		keeper->request(std::move(commands), [this, &got](std::vector<CommandResult> results) {
			log.push("reply");
			got = std::move(results);
		});
	}
	std::vector<CommandResult> run(std::vector<control::Command> commands) {
		std::optional<std::vector<CommandResult>> got;
		request(std::move(commands), got);
		pump();
		REQUIRE(got.has_value());
		return std::move(*got);
	}
	control::GraphFile graphFile() {
		auto results = run({control::GetGraphFile{}});
		REQUIRE(results.size() == 1);
		REQUIRE(std::holds_alternative<control::GraphFile>(results[0]));
		return std::get<control::GraphFile>(results[0]);
	}
	CommandResult update(const NodeName& node, json settings) {
		auto results = run({control::UpdateNode{node, std::move(settings)}});
		REQUIRE(results.size() == 1);
		return results[0];
	}
	CommandResult set(std::string text, std::string version) {
		auto results = run({control::SetGraphFile{std::move(text), std::move(version)}});
		REQUIRE(results.size() == 1);
		return results[0];
	}
	const control::GraphFileChanged& lastChange() const {
		REQUIRE_FALSE(fileLog.changes.empty());
		return fileLog.changes.back();
	}

	TempFolder dir;
	std::filesystem::path file;
	folder::ConfinedFolder folder;
	FolderGraphFileDisk realDisk{folder};
	NodeCatalogue catalogue{testTypes()};
	DesiredGraph graph{catalogue};
	Log log;
	GraphLog graphLog{log};
	FileLog fileLog{log};
	QueueExecutor main{&log};
	QueueExecutor self;
	Reader reader{graph, log};
	FakeOperations operations{log};
	ActionLog actions{log};
	FakeActions nodeActions{log};
	MainLoopSide side{graph, reader, operations};
	std::optional<Keeper> keeper;
};

bool ok(const CommandResult& result) {
	const auto* outcome = std::get_if<std::expected<void, CommandError>>(&result);
	return outcome != nullptr && outcome->has_value();
}

/// The data.code a refusal answers with ("" when it is not a refusal).
std::string code(const CommandResult& result) {
	if (const auto* outcome = std::get_if<std::expected<void, CommandError>>(&result))
		return outcome->has_value() ? "" : std::string(errorCode(outcome->error()));
	if (const auto* refusal = std::get_if<control::FileRefusal>(&result))
		return std::string(control::fileRefusalCode(*refusal));
	if (const auto* refusal = std::get_if<control::ControlRefusal>(&result))
		return std::string(control::controlRefusalCode(*refusal));
	return "";
}

/// What a lifecycle request applied ("" when it is not an Applied).
std::string applied(const CommandResult& result) {
	if (const auto* done = std::get_if<control::Applied>(&result)) return std::string(control::appliedName(done->kind));
	return "";
}

using Lines = std::vector<std::string>;

const json take2 = {{"path", "take2.wav"}};
const json take3 = {{"path", "take3.wav"}};
const std::string handText = replaced(recordExample, "take1.wav", "hand.wav");

} // namespace

TEST_CASE("start() offers the boot file: the file change first, then the commit's events", "[keeper]") {
	Fixture f("start");
	f.keeper->start();
	f.pump();
	REQUIRE(f.fileLog.changes.size() == 1); // the disk check after it finds the base unchanged
	const auto& change = f.fileLog.changes[0];
	CHECK(change.cause == FileChangeCause::disk);
	CHECK(change.droppedEdits == 0);
	CHECK(change.file.running == change.file.version);
	CHECK(change.file.text == recordExample);
	CHECK(change.file.diagnostics.empty());
	CHECK_FALSE(change.file.refusal.has_value());
	CHECK(f.log.lines == Lines{"file disk", "node.added desk", "node.added noise", "node.added porch", "node.added rec",
	                           "route.added noise:noise.mono desk:play.1",
	                           "route.added noise:noise.mono rec:record.left"});
}

TEST_CASE("start() reads ear.conf again once watching, so a hand edit made after the boot read is run", "[keeper]") {
	Fixture f("start_reread");  // the boot read: the record's example
	handEdit(f.file, handText); // before the watch is armed: no disk event will ever tell of it
	f.keeper->start();
	f.pump();
	REQUIRE(f.fileLog.changes.size() == 2);
	CHECK(f.fileLog.changes[0].file.text == recordExample);
	CHECK(f.lastChange().cause == FileChangeCause::disk);
	CHECK(f.lastChange().file.text == handText);
	CHECK(f.lastChange().file.running == f.lastChange().file.version);
	CHECK(f.graph.nodes()[3].spec.settings["path"] == "hand.wav");
}

TEST_CASE("A boot file with errors is announced with its diagnostics, and nothing runs", "[keeper]") {
	Fixture f("start_errors", replaced(recordExample, "node white noise;", "nod white noise;"));
	f.keeper->start();
	f.pump();
	CHECK(f.log.lines == Lines{"file disk"});
	const auto& change = f.lastChange();
	CHECK_FALSE(change.file.running.has_value());
	REQUIRE_FALSE(change.file.diagnostics.empty());
	CHECK(change.file.diagnostics[0].code == unknownDirectiveCode);
	CHECK(f.graph.nodes().empty());
}

TEST_CASE("A boot file the graph refuses shows the refusal at its place, and nothing runs", "[keeper]") {
	Fixture f("start_refused", replaced(recordExample, "rec:record.left", "rec:FX"));
	f.keeper->start();
	f.pump();
	CHECK(f.log.lines == Lines{"file disk"});
	const auto& change = f.lastChange();
	CHECK_FALSE(change.file.running.has_value());
	REQUIRE(change.file.refusal.has_value());
	CHECK(errorCode(change.file.refusal->error) == "unknown-port");
	CHECK(change.file.refusal->place == control::TextPlace{14, 1});
	CHECK(f.graph.nodes().empty());
}

TEST_CASE("An accepted edit is announced, then runs, then is answered, and is written when the "
          "keeper is free",
          "[keeper]") {
	Fixture f("edit");
	f.start();
	CHECK(ok(f.update("rec", take2)));
	CHECK(f.log.lines == Lines{"file edit", "node.updated rec", "reply"});
	const auto& change = f.lastChange();
	CHECK(change.file.running == change.file.version);
	CHECK(change.file.text == replaced(recordExample, "take1.wav", "take2.wav")); // the comment line kept
	CHECK(contents(f.file) == recordExample);                                     // not written yet

	f.free();
	CHECK(contents(f.file) == change.file.text);
	CHECK(f.fileLog.written == Lines{change.file.version});

	// playback 4 on desk rewrites @set $ch, so desk and porch are reshaped (their routes kept), in one commit.
	f.log.clear();
	CHECK(ok(f.update("desk", {{"card", "MOTU"}, {"playback", 4}})));
	CHECK(f.lastChange().file.text.find("@set $ch 4;") != std::string::npos);
	CHECK(f.log.lines == Lines{"file edit", "node.updated desk", "node.updated porch", "reply"});
	CHECK(std::all_of(f.log.turns.begin(), f.log.turns.end(), [&](int turn) { return turn == f.log.turns[0]; }));
}

TEST_CASE("A refused edit is answered and changes nothing: no announcement, no version, no write", "[keeper]") {
	Fixture f("refused");
	f.start();
	const auto before = f.graphFile();
	f.log.clear();
	auto results = f.run({control::AddRoute{{{"noise", "noise.mono"}, {"rec", "FX"}}}});
	CHECK(code(results[0]) == "unknown-port");
	CHECK(f.log.lines == Lines{"reply"});
	const auto after = f.graphFile();
	CHECK(after.version == before.version);
	CHECK(after.text == recordExample);
	f.free();
	CHECK(contents(f.file) == recordExample);
	CHECK(f.fileLog.written.empty());
}

TEST_CASE("A hand edit before the write wins: the unwritten edit is dropped and counted", "[keeper]") {
	Fixture f("collide_before");
	f.start();
	REQUIRE(ok(f.update("rec", take2)));
	f.log.clear();
	handEdit(f.file, handText);
	f.diskEvent();
	CHECK(f.log.lines == Lines{"file disk dropped=1", "node.updated rec"});
	const auto& change = f.lastChange();
	CHECK(change.file.text == handText);
	CHECK(change.file.running == change.file.version);
	CHECK(f.graph.nodes()[3].spec.settings["path"] == "hand.wav");
}

TEST_CASE("A hand edit at the swap wins: the write finds it, and leaves it in place", "[keeper]") {
	Fixture f("collide_swap");
	f.start();
	REQUIRE(ok(f.update("rec", take2)));
	f.log.clear();
	handEdit(f.file, handText); // no disk event: the write meets it
	f.free();
	CHECK(f.log.lines == Lines{"file disk dropped=1", "node.updated rec"});
	CHECK(f.lastChange().file.text == handText);
	CHECK(contents(f.file) == handText);
	CHECK(names(f.dir.path()) == Lines{"ear.conf"}); // no staging file left
	CHECK(f.fileLog.written.empty());
}

TEST_CASE("A refused hand edit after a dropped edit shows the refusal, and the graph keeps the "
          "dropped edit",
          "[keeper]") {
	Fixture f("refused_hand");
	f.start();
	REQUIRE(ok(f.update("rec", take2)));
	const auto edited = f.lastChange().file.version;
	f.log.clear();
	const auto hand = replaced(recordExample, "rec:record.left", "rec:FX");
	handEdit(f.file, hand);
	f.diskEvent();
	CHECK(f.log.lines == Lines{"file disk dropped=1"});
	const auto& change = f.lastChange();
	CHECK(change.droppedEdits == 1);
	REQUIRE(change.file.refusal.has_value());
	CHECK(errorCode(change.file.refusal->error) == "unknown-port");
	CHECK(change.file.refusal->place == control::TextPlace{14, 1});
	CHECK(change.file.running == edited);
	CHECK(f.graphFile().text == hand);
	f.free();
	CHECK(contents(f.file) == hand);
	CHECK(f.fileLog.written.empty());
}

TEST_CASE("A hand typo is shown and runs nothing, and edits are then file-invalid until a set fixes it", "[keeper]") {
	Fixture f("typo");
	f.start();
	handEdit(f.file, replaced(recordExample, "node white noise;", "nod white noise;"));
	f.diskEvent();
	CHECK(f.log.lines == Lines{"file disk"});
	REQUIRE_FALSE(f.lastChange().file.diagnostics.empty());
	CHECK(f.lastChange().file.diagnostics[0].severity == control::Severity::error);
	CHECK(code(f.update("rec", take2)) == "file-invalid");

	f.log.clear();
	CHECK(ok(f.set(replaced(recordExample, "take1.wav", "fixed.wav"), f.lastChange().file.version)));
	CHECK(f.log.lines == Lines{"file edit", "node.updated rec", "reply"});
	CHECK(f.lastChange().file.diagnostics.empty());
}

TEST_CASE("graphFile.set: a stale version, a text with errors, the same text and a good text", "[keeper]") {
	Fixture f("set");
	f.start();
	const auto version = f.graphFile().version;
	f.log.clear();

	CHECK(code(f.set(handText, version + "0")) == "file-changed");

	auto bad = f.set(replaced(recordExample, "node white noise;", "nod white noise;"), version);
	CHECK(code(bad) == "invalid-text");
	REQUIRE(std::holds_alternative<control::FileRefusal>(bad));
	const auto& refusal = std::get<control::FileRefusal>(bad);
	REQUIRE(std::holds_alternative<control::InvalidText>(refusal));
	CHECK(std::get<control::InvalidText>(refusal).diagnostics[0].code == unknownDirectiveCode);
	CHECK(f.graphFile().version == version); // nothing kept
	CHECK(f.fileLog.changes.size() == 1);    // the boot offer's only

	f.log.clear();
	CHECK(ok(f.set(recordExample, version)));
	CHECK(f.log.lines == Lines{"reply"}); // nothing announced

	f.log.clear();
	CHECK(ok(f.set(handText, version)));
	CHECK(f.log.lines == Lines{"file edit", "node.updated rec", "reply"});
	CHECK(f.graphFile().text == handText);
}

TEST_CASE("node.setText: a stale version first, file-invalid, then the text's own checks, and accepted it is an edit",
          "[keeper][node-text]") {
	Fixture f("set_text");
	f.start();
	const auto boot = f.graphFile();
	REQUIRE(boot.nodeTexts.has_value()); // the file's state carries where each node is written
	CHECK(boot.nodeTexts->front() == control::NodeText{"noise", {10, 1}, {10, 18}, false});
	const auto setText = [&](std::string node, std::string text, std::string version) {
		auto results = f.run({control::SetNodeText{std::move(node), std::move(text), std::move(version)}});
		REQUIRE(results.size() == 1);
		return results[0];
	};
	f.log.clear();

	// A stale version is file-changed even with a text that would be refused for itself.
	CHECK(code(setText("rec", "oops", boot.version + "0")) == "file-changed");
	CHECK(code(setText("nope", "node white nope;", boot.version)) == "unknown-node");
	CHECK(code(setText("rec", "node file rec { path take2.wav }", boot.version)) == "invalid-text");
	CHECK(f.log.lines == Lines{"reply", "reply", "reply"}); // nothing announced, nothing run
	f.log.clear();

	CHECK(ok(setText("rec", R"(node file rec { path "take2.wav"; })", boot.version)));
	CHECK(f.log.lines == Lines{"file edit", "node.updated rec", "reply"});
	CHECK(f.lastChange().cause == FileChangeCause::edit);
	CHECK(f.lastChange().file.text == replaced(recordExample, "take1.wav", "take2.wav"));
	CHECK(f.lastChange().file.nodeTexts == boot.nodeTexts); // the same length: every node where it was

	handEdit(f.file, replaced(recordExample, "node white noise;", "nod white noise;"));
	f.diskEvent();
	CHECK_FALSE(f.lastChange().file.nodeTexts.has_value()); // a file with errors says where no node is
	CHECK(code(setText("rec", R"(node file rec { path "take3.wav"; })", f.lastChange().file.version)) ==
	      "file-invalid");
}

TEST_CASE("node.addText: a stale version first, file-invalid, then the text's own checks, and accepted it is an edit",
          "[keeper][node-text]") {
	Fixture f("add_text");
	f.start();
	const auto boot = f.graphFile();
	const auto addText = [&](std::string text, std::string version) {
		auto results = f.run({control::AddNodeText{std::move(text), std::move(version)}});
		REQUIRE(results.size() == 1);
		return results[0];
	};
	f.log.clear();

	CHECK(code(addText("oops", boot.version + "0")) == "file-changed");
	CHECK(code(addText("node white a; node white b;", boot.version)) == "invalid-text");
	CHECK(code(addText("node white noise;", boot.version)) == "duplicate-node"); // the graph's to judge
	CHECK(f.log.lines == Lines{"reply", "reply", "reply"});                      // nothing announced, nothing run
	f.log.clear();

	CHECK(ok(addText("node white n2;", boot.version)));
	CHECK(f.log.lines == Lines{"file edit", "node.added n2", "reply"});
	CHECK(f.lastChange().file.text == recordExample + "node white n2;\n");
	CHECK(f.lastChange().file.nodeTexts->back() == control::NodeText{"n2", {16, 1}, {16, 15}, false});

	handEdit(f.file, replaced(recordExample, "node white noise;", "nod white noise;"));
	f.diskEvent();
	CHECK(code(addText("node white n3;", f.lastChange().file.version)) == "file-invalid");
}

TEST_CASE("The watch event for the keeper's own write announces nothing", "[keeper]") {
	Fixture f("own_write");
	f.start();
	REQUIRE(ok(f.update("rec", take2)));
	f.free();
	REQUIRE(f.fileLog.written.size() == 1);
	f.log.clear();
	f.diskEvent();
	CHECK(f.log.lines.empty());
}

TEST_CASE("A batch's reads come after its edit, in one main-loop turn with the reply", "[keeper]") {
	Fixture f("batch");
	f.start();
	auto results = f.run({control::UpdateNode{"rec", take2}, control::GetGraph{}, control::GetGraphFile{}});
	CHECK(f.log.lines == Lines{"file edit", "node.updated rec", "read graph", "reply"});
	CHECK(f.log.turnOf("read graph") == f.log.turnOf("reply"));
	CHECK(f.log.turnOf("node.updated rec") < f.log.turnOf("reply"));
	REQUIRE(results.size() == 3);
	CHECK(ok(results[0]));
	REQUIRE(std::holds_alternative<GraphView>(results[1]));
	CHECK(std::get<GraphView>(results[1]).nodes[3].spec.settings == take2);
	REQUIRE(std::holds_alternative<control::GraphFile>(results[2]));
	CHECK(std::get<control::GraphFile>(results[2]).version == f.lastChange().file.version);
	CHECK(std::get<control::GraphFile>(results[2]).running == f.lastChange().file.version);
}

TEST_CASE("start, stop and restart reach the main-loop side in order with edits, write nothing, make no version, and "
          "work while the file has errors",
          "[keeper][lifecycle]") {
	Fixture f("verbs");
	f.start();
	const auto before = f.graphFile();
	f.log.clear();
	f.operations.reply = LifecycleApplied::stop;
	auto results = f.run(
		{control::StopNode{"rec"}, control::UpdateNode{"rec", take2}, control::StartNode{"rec"}, control::GetGraph{}});
	// The stop before the edit goes first, in the edit's step; the start after it waits for the edit's commit.
	CHECK(f.log.lines == Lines{"stop rec", "action rec stop ok", "health rec", "file edit", "node.updated rec",
	                           "start rec", "action rec start ok", "health rec", "read graph", "reply"});
	CHECK(f.log.turnOf("stop rec") == f.log.turnOf("file edit"));
	CHECK(f.log.turnOf("start rec") == f.log.turnOf("reply"));
	REQUIRE(results.size() == 4);
	CHECK(applied(results[0]) == "stop");
	CHECK(ok(results[1]));
	CHECK(applied(results[2]) == "stop"); // the fake's one reply
	f.free();
	const auto edited = f.graphFile();
	CHECK(contents(f.file) == edited.text);

	// A verb alone: no version, no write, no announcement.
	f.log.clear();
	f.operations.reply = LifecycleApplied::restart;
	results = f.run({control::RestartNode{"desk"}});
	CHECK(f.log.lines == Lines{"restart desk", "action desk restart ok", "health desk", "reply"});
	CHECK(applied(results[0]) == "restart");
	CHECK(f.graphFile().version == edited.version);
	f.free();
	CHECK(f.fileLog.written.size() == 1); // the edit's only

	// While the file has errors, edits are file-invalid but the verbs still work.
	handEdit(f.file, replaced(edited.text, "node white noise;", "nod white noise;"));
	f.diskEvent();
	f.log.clear();
	f.operations.reply = LifecycleApplied::start;
	results = f.run({control::StartNode{"rec"}, control::RemoveNode{"rec"}, control::DisableNode{"rec"}});
	CHECK(applied(results[0]) == "start");
	CHECK(code(results[1]) == "file-invalid");
	CHECK(code(results[2]) == "file-invalid");
	CHECK(f.log.lines == Lines{"start rec", "action rec start ok", "health rec", "reply"});
	CHECK(before.version != edited.version);
}

TEST_CASE("Once the keeper stopped, a verb still reaches the main loop and answers stopping with its action record, "
          "without asking NodeHost",
          "[keeper][lifecycle]") {
	Fixture f("verbs_stopped");
	f.start();
	f.keeper->stop();
	f.pump();
	f.log.clear();
	const auto results = f.run({control::StopNode{"rec"}, control::DisableNode{"rec"}, control::GetGraph{}});
	CHECK(f.log.lines == Lines{"action rec stop stopping", "read graph", "reply"});
	CHECK(code(results[0]) == "stopping");
	CHECK(code(results[1]) == "stopping");
	REQUIRE(std::holds_alternative<GraphView>(results[2]));
}

TEST_CASE("node.disable and node.enable are edits at the node's own line, answered by the plan's outcome",
          "[keeper][lifecycle]") {
	Fixture f("disable");
	f.start();
	auto results = f.run({control::DisableNode{"rec"}});
	CHECK(applied(results[0]) == "disable");
	CHECK(f.log.lines == Lines{"file edit", "node.updated rec", "reply"}); // no action record: an edit says it
	CHECK(f.lastChange().file.text == replaced(recordExample, R"(node file rec { path "take1.wav"; })",
	                                           R"(node file rec { disabled true; path "take1.wav"; })"));
	CHECK(f.graph.nodes()[3].spec.disabled);
	f.free();
	CHECK(contents(f.file) == f.lastChange().file.text);

	f.log.clear();
	const auto version = f.graphFile().version;
	results = f.run({control::DisableNode{"rec"}}); // already disabled: nothing to edit
	CHECK(applied(results[0]) == "unchanged");
	CHECK(f.graphFile().version == version);

	results = f.run({control::UpdateNode{"rec", take2}}); // kept for the enable
	CHECK(ok(results[0]));
	results = f.run({control::EnableNode{"rec"}});
	CHECK(applied(results[0]) == "enable");
	CHECK(f.lastChange().file.text == replaced(recordExample, "take1.wav", "take2.wav"));
	CHECK_FALSE(f.graph.nodes()[3].spec.disabled);
	results = f.run({control::EnableNode{"rec"}});
	CHECK(applied(results[0]) == "unchanged");

	results = f.run({control::DisableNode{"ghost"}});
	CHECK(code(results[0]) == "unknown-node");
	results = f.run({control::DisableNode{"desk"}}); // its line is its own: only desk is disabled
	CHECK(applied(results[0]) == "disable");
	CHECK(f.graph.nodes()[0].spec.disabled);
	CHECK_FALSE(f.graph.nodes()[2].spec.disabled);
}

TEST_CASE("A disk event while an offer awaits its verdict is handled after the verdict", "[keeper]") {
	Fixture f("await");
	f.start();
	std::optional<std::vector<CommandResult>> got;
	f.request({control::UpdateNode{"rec", take2}}, got); // its step waits on the main loop
	handEdit(f.file, handText);
	f.keeper->diskEvent();
	f.pump();
	CHECK(f.log.lines == Lines{"file edit", "node.updated rec", "reply", "file disk dropped=1", "node.updated rec"});
	CHECK(f.graphFile().text == handText);
}

TEST_CASE("Edits that arrive before the keeper is free share one write", "[keeper]") {
	Fixture f("coalesce");
	f.start();
	std::optional<std::vector<CommandResult>> first;
	std::optional<std::vector<CommandResult>> second;
	f.request({control::UpdateNode{"rec", take2}}, first);
	f.request({control::UpdateNode{"rec", take3}}, second);
	f.pump();
	REQUIRE(first.has_value());
	REQUIRE(second.has_value());
	CHECK(ok((*second)[0]));
	f.free();
	CHECK(f.fileLog.written.size() == 1);
	CHECK(contents(f.file) == replaced(recordExample, "take1.wav", "take3.wav"));
}

TEST_CASE("A failed write is shown, retried only after the next accepted change, and cleared by success", "[keeper]") {
	FakeDisk disk(recordExample);
	Fixture f("write_failed", recordExample, &disk);
	f.start();
	REQUIRE(ok(f.update("rec", take2)));
	disk.failing = true;
	f.log.clear();
	f.free();
	CHECK(f.log.lines == Lines{"file write"});
	CHECK(f.lastChange().file.writeFailed == "disk full");
	CHECK(disk.replaces == 1);

	f.graphFile(); // a read wakes the keeper, and retries nothing
	f.free();
	CHECK(disk.replaces == 1);

	disk.failing = false;
	REQUIRE(ok(f.update("rec", take3)));
	CHECK(f.lastChange().file.writeFailed == "disk full"); // still failed until a write succeeds
	f.log.clear();
	f.free();
	CHECK(disk.replaces == 2);
	CHECK(f.log.lines == Lines{"file write", "written"});
	CHECK_FALSE(f.lastChange().file.writeFailed.has_value());
	CHECK(disk.text == replaced(recordExample, "take1.wav", "take3.wav"));
}

TEST_CASE("A write failure is announced again only when its detail changes", "[keeper]") {
	FakeDisk disk(recordExample);
	Fixture f("write_failed_again", recordExample, &disk);
	f.start();
	disk.failing = true;
	REQUIRE(ok(f.update("rec", take2)));
	f.free();
	REQUIRE(f.lastChange().cause == FileChangeCause::write);

	REQUIRE(ok(f.update("rec", take3))); // the next accepted change retries the write
	f.log.clear();
	f.free();
	CHECK(disk.replaces == 2);
	CHECK(f.log.lines.empty()); // the same failure: nothing new to say

	disk.failure = "read-only file system";
	REQUIRE(ok(f.update("rec", take2)));
	f.log.clear();
	f.free();
	CHECK(disk.replaces == 3);
	CHECK(f.log.lines == Lines{"file write"});
	CHECK(f.lastChange().file.writeFailed == "read-only file system");
}

TEST_CASE("The version moves only when the text changes, so a reload of the same text keeps a set valid", "[keeper]") {
	Fixture f("same_text");
	f.start();
	REQUIRE(ok(f.update("rec", take2)));
	const auto edited = f.graphFile();
	handEdit(f.file, edited.text); // a hand save of exactly the working copy, before ear wrote it
	f.diskEvent();
	const auto reloaded = f.graphFile();
	CHECK(reloaded.text == edited.text);
	CHECK(reloaded.version == edited.version);
	CHECK(ok(f.set(replaced(edited.text, "take2.wav", "take3.wav"), edited.version)));
}

TEST_CASE("An edit that would make ear.conf larger than ear reads is refused as invalid-text, and "
          "nothing is kept",
          "[keeper]") {
	Fixture f("too_large");
	f.start();
	const std::string longPath(maxDocumentBytes / 2, 'a');
	REQUIRE(ok(f.update("rec", {{"path", longPath}})));
	const auto before = f.graphFile();
	f.log.clear();
	auto results = f.run({control::AddNode{{"rec2", "file", {{"path", longPath}}}}});
	CHECK(code(results[0]) == "invalid-text");
	REQUIRE(std::holds_alternative<control::FileRefusal>(results[0]));
	const auto& refused = std::get<control::FileRefusal>(results[0]);
	REQUIRE(std::holds_alternative<control::InvalidText>(refused));
	CHECK(std::get<control::InvalidText>(refused).diagnostics[0].code == tooLargeCode);
	CHECK(f.log.lines == Lines{"reply"});
	CHECK(f.graphFile().version == before.version);
	f.free();
	CHECK(contents(f.file) == before.text); // the last text that fits is the one written
}

TEST_CASE("A file at another rate is accepted with a pending restart, and a missing file is "
          "created by a set",
          "[keeper]") {
	SECTION("pending restart") {
		Fixture f("pending", recordExample, nullptr, {ear::SampleRate::fixed<48000>(), 256});
		f.start();
		const auto other = replaced(recordExample, "rate 48khz;", "rate 44.1khz;");
		CHECK(ok(f.set(other, f.graphFile().version)));
		CHECK(f.lastChange().file.pendingRestart ==
		      std::vector<control::FormatKey>{control::FormatKey::graphFrequency});
	}
	SECTION("missing") {
		Fixture f("missing", std::nullopt);
		f.keeper->start();
		f.pump();
		const auto& boot = f.lastChange();
		CHECK(boot.file.text.empty());
		REQUIRE(boot.file.diagnostics.size() == 1);
		CHECK(boot.file.diagnostics[0].code == missingFileCode);
		CHECK_FALSE(boot.file.running.has_value());
		CHECK(ok(f.set(recordExample, boot.file.version)));
		f.free();
		CHECK(contents(f.file) == recordExample);
		CHECK(f.fileLog.written.size() == 1);
	}
}

TEST_CASE("stop(): queued edits answer stopping, reads are answered, and the last accepted version "
          "is written",
          "[keeper]") {
	SECTION("the last write") {
		Fixture f("stop");
		f.start();
		std::optional<std::vector<CommandResult>> edit;
		f.request({control::UpdateNode{"rec", take2}}, edit);
		f.main.runOne(); // judged and accepted; its completion waits on the keeper's thread
		// The graph stays open, so the late edit's stopping is the keeper's own answer, not the plan's.
		std::optional<std::vector<CommandResult>> late;
		std::optional<std::vector<CommandResult>> read;
		f.request({control::UpdateNode{"rec", take3}}, late);
		f.request({control::GetGraph{}}, read);
		f.keeper->stop();
		f.pump();
		REQUIRE(edit.has_value());
		CHECK(ok((*edit)[0]));
		REQUIRE(late.has_value());
		CHECK(code((*late)[0]) == "stopping");
		REQUIRE(read.has_value());
		CHECK(std::holds_alternative<GraphView>((*read)[0]));
		CHECK(contents(f.file) == replaced(recordExample, "take1.wav", "take2.wav"));
		CHECK_FALSE(f.keeper->finalWriteFailure().has_value());

		CHECK(f.graph.nodes()[3].spec.settings == take2);

		// A text with errors is announced without a plan, so only the stop guards keep it unread.
		f.log.clear();
		handEdit(f.file, replaced(recordExample, "node white noise;", "nod white noise;"));
		f.diskEvent();
		CHECK(f.log.lines.empty());
		CHECK(f.main.empty());
	}
	SECTION("a failing last write") {
		FakeDisk disk(recordExample);
		Fixture f("stop_failed", recordExample, &disk);
		f.start();
		REQUIRE(ok(f.update("rec", take2)));
		disk.failing = true;
		f.graph.close();
		f.keeper->stop();
		f.pump();
		CHECK(f.keeper->finalWriteFailure() == "disk full");
	}
}

namespace {

const std::string threeChannels = R"(graph {
    rate 48khz;
    block_size 256;
}
node white noise;
node card desk { card MOTU; playback 3; }
node card porch { card USB2; playback 2; }
route noise:noise.mono desk:play.1;
route noise:noise.mono desk:play.2; # goes with desk's second channel
route noise:noise.mono porch:play.2;
route noise:noise.mono desk:play.3;
)";

} // namespace

TEST_CASE("An update dropping ports that routes name is refused ports-in-use, naming every such route, and changes "
          "nothing",
          "[keeper][reshape]") {
	Fixture f("ports_in_use", threeChannels);
	f.start();
	const auto before = f.graphFile();
	f.log.clear();
	const CommandResult result = f.update("desk", {{"card", "MOTU"}, {"playback", 1}});
	REQUIRE(code(result) == "ports-in-use");
	const auto& inUse = std::get<control::PortsInUse>(std::get<control::ControlRefusal>(result));
	CHECK(inUse.node == "desk");
	CHECK(inUse.ports == std::vector<PortName>{"play.2", "play.3"});
	CHECK(inUse.routes == std::vector<Route>{{{"noise", "noise.mono"}, {"desk", "play.2"}},
	                                         {{"noise", "noise.mono"}, {"desk", "play.3"}}});
	CHECK(f.log.lines == Lines{"reply"});
	CHECK(f.graphFile().version == before.version);
	// A change that strands no route is not refused: the channel it adds is unwired.
	CHECK(ok(f.update("desk", {{"card", "MOTU"}, {"playback", 4}})));
}

TEST_CASE("dropRoutes removes exactly the stranded route lines with the change, in one commit and one write",
          "[keeper][reshape]") {
	Fixture f("drop_routes", threeChannels);
	f.start();
	const std::size_t changes = f.fileLog.changes.size();
	auto results = f.run({control::UpdateNode{"desk", {{"card", "MOTU"}, {"playback", 1}}, true}});
	CHECK(ok(results[0]));
	CHECK(f.log.lines == Lines{"file edit", "route.removed noise:noise.mono desk:play.2",
	                           "route.removed noise:noise.mono desk:play.3", "node.updated desk", "reply"});
	const std::string expected =
		replaced(replaced(replaced(threeChannels, "playback 3;", "playback 1;"),
	                      "route noise:noise.mono desk:play.2; # goes with desk's second channel\n", ""),
	             "route noise:noise.mono desk:play.3;\n", "");
	CHECK(f.lastChange().file.text == expected);
	CHECK(f.fileLog.changes.size() == changes + 1);
	f.free();
	CHECK(contents(f.file) == expected);
	CHECK(f.fileLog.written.size() == 1);
}

TEST_CASE("dropRoutes refuses shared-definition when a stranded route's line makes other routes too, and changes "
          "nothing",
          "[keeper][reshape]") {
	const std::string snippet = R"(graph {
    rate 48khz;
    block_size 256;
}
@snippet both $to {
    route noise:noise.mono $to;
}
node white noise;
node card desk { card MOTU; playback 2; }
@use both desk:play.1;
@use both desk:play.2;
)";
	Fixture f("drop_shared", snippet);
	f.start();
	const auto before = f.graphFile();
	f.log.clear();
	auto results = f.run({control::UpdateNode{"desk", {{"card", "MOTU"}, {"playback", 1}}, true}});
	CHECK(code(results[0]) == "shared-definition");
	CHECK(f.log.lines == Lines{"reply"});
	CHECK(f.graphFile().text == before.text);
}

namespace {

const control::Applied& answered(const CommandResult& result) {
	REQUIRE(std::holds_alternative<control::Applied>(result));
	return std::get<control::Applied>(result);
}

} // namespace

TEST_CASE("node.set merges into the node's settings, null removing a key, and answers what the plan applied",
          "[keeper][node-set]") {
	Fixture f("node_set", threeChannels);
	f.start();
	auto results = f.run({control::SetNode{"desk", {{"card", "X"}}}});
	CHECK(answered(results[0]).kind == control::AppliedKind::restart);
	CHECK_FALSE(answered(results[0]).ports.has_value());
	CHECK(f.lastChange().file.text == replaced(threeChannels, "card MOTU;", "card \"X\";")); // playback 3 kept

	f.log.clear();
	results = f.run({control::SetNode{"desk", {{"capture", 2}}}});
	const control::Applied& added = answered(results[0]);
	CHECK(added.kind == control::AppliedKind::reshape);
	CHECK(added.inPlace == false);
	CHECK(added.ports == PortsMoved{{"capture.1", "capture.2"}, {}});
	CHECK(added.routesDropped == std::vector<Route>{});
	CHECK_FALSE(added.routes.has_value());
	CHECK(f.lastChange().file.text.find("capture 2;") != std::string::npos);

	results = f.run({control::SetNode{"desk", {{"capture", nullptr}}}}); // back to its default, 0
	CHECK(answered(results[0]).ports == PortsMoved{{}, {"capture.1", "capture.2"}});
	CHECK(f.lastChange().file.text.find("capture") == std::string::npos);
	CHECK(f.graph.nodes()[0].spec.settings == json{{"card", "X"}, {"playback", 3}});

	f.log.clear();
	results = f.run({control::SetNode{"desk", {{"playback", 3}}}});
	CHECK(answered(results[0]).kind == control::AppliedKind::unchanged);
	CHECK(f.log.lines == Lines{"reply"});
}

TEST_CASE("node.set of a live key is a retune, naming its retuned keys and the other nodes a shared definition moved",
          "[keeper][node-set][live]") {
	const std::string faders = R"(graph {
    rate 48khz;
    block_size 256;
}
@set $g -6;
node fader a { gainDb $g; }
node fader b { gainDb $g; }
node fader c;
)";
	Fixture f("node_set_live", faders);
	f.start();
	auto results = f.run({control::SetNode{"a", {{"gainDb", -3.5}}, true}});
	const control::Applied& dry = answered(results[0]);
	CHECK(dry.kind == control::AppliedKind::retune);
	CHECK(dry.retuned == std::vector<std::string>{"gainDb"});
	CHECK(dry.alsoChanged == std::vector<control::NodeKey>{{"b", "gainDb"}});
	CHECK(f.graphFile().text == faders); // a dry run changes nothing

	results = f.run({control::SetNode{"a", {{"gainDb", -3.5}}}});
	const control::Applied& shared = answered(results[0]);
	CHECK(shared.kind == control::AppliedKind::retune);
	CHECK(shared.retuned == std::vector<std::string>{"gainDb"});
	CHECK(shared.alsoChanged == std::vector<control::NodeKey>{{"b", "gainDb"}});
	CHECK(f.lastChange().file.text.find("@set $g -3.5;") != std::string::npos);

	results = f.run({control::SetNode{"c", {{"mute", true}, {"gainDb", -1}}}});
	const control::Applied& own = answered(results[0]);
	CHECK(own.kind == control::AppliedKind::retune);
	CHECK(own.retuned == std::vector<std::string>{"gainDb", "mute"});
	CHECK(own.alsoChanged == std::vector<control::NodeKey>{});

	results = f.run({control::SetNode{"c", {{"mute", true}}}});
	const control::Applied& same = answered(results[0]);
	CHECK(same.kind == control::AppliedKind::unchanged);
	CHECK(same.retuned == std::vector<std::string>{});
	CHECK(same.alsoChanged == std::vector<control::NodeKey>{});
}

TEST_CASE("node.set refuses a required key removed, an unknown node and settings that are not an object",
          "[keeper][node-set]") {
	Fixture f("node_set_refused");
	f.start();
	auto results = f.run({control::SetNode{"rec", {{"path", nullptr}}}, control::SetNode{"ghost", json::object()},
	                      control::SetNode{"rec", json::array()}});
	CHECK(code(results[0]) == "invalid-settings");
	CHECK(describe(std::get<std::expected<void, CommandError>>(results[0]).error()) ==
	      "invalid-settings rec: path: missing");
	CHECK(code(results[1]) == "unknown-node");
	CHECK(code(results[2]) == "invalid-settings");
	CHECK(f.log.lines == Lines{"reply"});
	CHECK(f.graphFile().text == recordExample);
}

TEST_CASE("A dry run answers as the real edit would, names the routes a reshape would remove, and changes nothing "
          "(no announcement, version, graph change or write)",
          "[keeper][node-set]") {
	Fixture f("dry_run", threeChannels);
	f.start();
	const auto before = f.graphFile();
	f.log.clear();
	const std::vector<Route> stranded{{{"noise", "noise.mono"}, {"desk", "play.2"}},
	                                  {{"noise", "noise.mono"}, {"desk", "play.3"}}};
	auto results =
		f.run({control::SetNode{"desk", {{"playback", 1}}, true}, control::SetNode{"desk", {{"card", "X"}}, true},
	           control::SetNode{"desk", {{"playback", 3}}, true}});
	const control::Applied& reshape = answered(results[0]);
	CHECK(reshape.kind == control::AppliedKind::reshape);
	CHECK(reshape.inPlace == false);
	CHECK(reshape.ports == PortsMoved{{}, {"play.2", "play.3"}});
	CHECK(reshape.routes == stranded);
	CHECK_FALSE(reshape.routesDropped.has_value());
	CHECK(answered(results[1]).kind == control::AppliedKind::restart);
	CHECK(answered(results[2]).kind == control::AppliedKind::unchanged);
	CHECK(f.log.lines == Lines{"reply"}); // nothing told, nothing committed
	CHECK(f.fileLog.changes.size() == 1); // the boot's
	const auto after = f.graphFile();
	CHECK(after.version == before.version);
	CHECK(after.text == threeChannels);
	CHECK(f.graph.routes().size() == 4);
	f.free();
	CHECK(f.fileLog.written.empty());
	CHECK(contents(f.file) == threeChannels);

	// The real edit is judged afresh: refused without dropRoutes, made with it.
	results = f.run({control::SetNode{"desk", {{"playback", 1}}}});
	CHECK(code(results[0]) == "ports-in-use");
	results = f.run({control::SetNode{"desk", {{"playback", 1}}, false, true}});
	CHECK(answered(results[0]).routesDropped == stranded);
	CHECK_FALSE(answered(results[0]).routes.has_value());
	CHECK(f.graph.routes().size() == 2);
}

TEST_CASE("An invoke between two edits of one batch runs between them, and the batch's reply waits for its answer, "
          "behind every event before it",
          "[keeper][actions]") {
	Fixture f("invoke_between");
	f.start();
	f.nodeActions.hold = true;
	std::optional<std::vector<CommandResult>> got;
	f.request({control::UpdateNode{"rec", take2}, control::InvokeNode{"rec", "reset", nullptr},
	           control::UpdateNode{"rec", take3}},
	          got);
	f.pump();
	CHECK(f.log.lines ==
	      Lines{"file edit", "node.updated rec", "invoke rec reset null", "file edit", "node.updated rec"});
	CHECK(f.log.turnOf("invoke rec reset null") < f.log.lines.size()); // asked in the second edit's step
	CHECK_FALSE(got.has_value());
	// The keeper's queue is not held: another batch is taken and answered meanwhile.
	auto other = f.run({control::GetGraph{}});
	CHECK(other.size() == 1);
	CHECK_FALSE(got.has_value());
	f.nodeActions.answer({});
	f.pump();
	REQUIRE(got.has_value());
	REQUIRE(got->size() == 3);
	CHECK(ok((*got)[0]));
	CHECK(ok((*got)[1]));
	CHECK(ok((*got)[2]));
	CHECK(f.log.lines.back() == "reply");
	CHECK(f.actions.records.back() == control::ActionRecord{"rec", "reset", "ok", std::nullopt});
}

TEST_CASE("A slow handler holds only its own batch: the keeper takes the next one, and each reply comes when its "
          "batch is done",
          "[keeper][actions]") {
	Fixture f("invoke_slow");
	f.start();
	f.nodeActions.hold = true;
	std::optional<std::vector<CommandResult>> slow;
	std::optional<std::vector<CommandResult>> edit;
	f.request({control::InvokeNode{"rec", "forget", json{{"server", 3}}}}, slow);
	f.request({control::UpdateNode{"rec", take2}}, edit);
	f.pump();
	CHECK_FALSE(slow.has_value());
	REQUIRE(edit.has_value());
	CHECK(ok((*edit)[0]));
	f.nodeActions.answer(
		std::unexpected(ActionRefusal{.code = ActionRefusal::Code::failed, .detail = "no such server"}));
	f.pump();
	REQUIRE(slow.has_value());
	CHECK(code((*slow)[0]) == "action-failed");
	CHECK(f.actions.records.back() ==
	      control::ActionRecord{"rec", "forget", "action-failed", std::string("no such server")});
}

TEST_CASE("An action answered at once is answered in its batch's last step, its record first, and once the keeper "
          "stopped it answers stopping without asking",
          "[keeper][actions]") {
	Fixture f("invoke_now");
	f.start();
	f.nodeActions.reply =
		std::unexpected(ActionRefusal{.code = ActionRefusal::Code::notRunning, .phase = NodePhase::stopped});
	auto results = f.run({control::InvokeNode{"rec", "reset", nullptr}, control::GetGraph{}});
	CHECK(f.log.lines == Lines{"invoke rec reset null", "action rec reset not-running stopped", "read graph", "reply"});
	CHECK(code(results[0]) == "not-running");
	f.keeper->stop();
	f.pump();
	f.log.clear();
	results = f.run({control::InvokeNode{"rec", "reset", nullptr}});
	CHECK(f.log.lines == Lines{"action rec reset stopping", "reply"});
	CHECK(code(results[0]) == "stopping");
}
