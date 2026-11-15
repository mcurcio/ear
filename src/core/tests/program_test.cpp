// Programs, pipe receivers and AnyTcp claims (docs/writing-a-node-type.md#programs, #engine-companions, #claims): the
// pure rules (a pipe receiver's delay, the kinds' names, a program's words and paths, its arguments as a type writes
// them), then a type of this file's own through the harness's real NodeHost: a pipe receiver on an inside input bank, a
// port picked for it, and one program naming both, with a file and a report feed into a strand's state. Two nodes'
// ports, and hosts with no adapter, are node_host_test.cpp's.
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/companions.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/program.hpp"
#include "ear/testing/type_harness.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace program_test {

using namespace ear;
using namespace std::chrono_literals;

/// What the report feed delivered to the state, in order (the harness runs the strand on the test's thread).
std::string fed;

/// The strand's state: the feed's chunks land here.
struct Reports {
	void onFeed(std::string_view bytes) { fed += bytes; }
};

/// Which rule configure breaks, when one.
enum class Broken {
	none,
	secondProgram,
	badFileName,
	feedFileName,
	twinFileName,
	tooManyFiles,
	feedWithoutFeed,
	unknownFile,
	foreignPort,
	fifoWithoutReceiver,
	tooManyNeeds,
	twinNeeds,
};
struct Behaviour {
	Broken broken = Broken::none;
	bool pipe = true;                 // configure asks for the pipe receiver
	std::vector<std::string> needs{}; // the shared-memory names the program needs
};
Behaviour behaviour;

constexpr SettingsTable<> table{"piped"};
constexpr auto out = bank::output<"out">;
constexpr auto fromPipe = bank::insideInput<"from_pipe">;
constexpr auto reports = loop::strand<"reports", Reports>;
constexpr TypeSpec spec{.settings = table, .banks = Banks<out, fromPipe>{}, .loops = Loops<reports>{}};
using Checked = CheckedOf<spec>;

constexpr ProgramPath player{"/usr/local/bin/fake-player"};

struct Copy {
	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (std::size_t member = 0; member < io[out].size(); ++member)
			for (std::uint32_t frame = 0; frame < io.frames(); ++frame)
				io[out][member][frame] = io[fromPipe][member][frame];
	}
};

class Piped {
public:
	static Members<spec> ports(const Checked&) { return {out(members(2)), fromPipe(members(2))}; }
	Configured configure(CreateOf<spec>& cx, const Checked&) {
		if (behaviour.pipe) cx.pipeReceiver(fromPipe, {PipeFormat::s16le, SampleRate::fixed<44100>()});
		const GrantedPort port = cx.claim(claim::AnyTcp{});
		ProgramShape shape{
			.executable = player,
			.args = {"--port", {part::Port{port}}, {part::Text{"--config="}, part::File{"config"}}},
			.files = {ProgramFile{
				"config",
				{part::Text{"pipe = "}, cx.fifoOf(fromPipe), part::Text{"\nfeed = "}, part::Feed{}, part::Text{"\n"}}}},
		};
		for (const std::string& need : behaviour.needs) shape.needs.push_back(*SharedMemoryName::parse(need));
		switch (behaviour.broken) {
		case Broken::none:
		case Broken::secondProgram:
		case Broken::fifoWithoutReceiver:
			break;
		case Broken::badFileName:
			shape.files.push_back({"Bad_Name", {}});
			break;
		case Broken::feedFileName:
			shape.files.push_back({"feed", {}});
			break;
		case Broken::twinFileName:
			shape.files.push_back({"config", {}});
			break;
		case Broken::tooManyFiles:
			for (int i = 0; i < 8; ++i) shape.files.push_back({"f" + std::to_string(i), {}});
			break;
		case Broken::feedWithoutFeed:
			cx.program(std::move(shape));
			cx.use(Copy{});
			return Configured::applied();
		case Broken::unknownFile:
			shape.args.push_back({part::File{"missing"}});
			break;
		case Broken::foreignPort:
			shape.args.push_back({part::Port{GrantedPort(7)}});
			break;
		case Broken::tooManyNeeds:
			for (const char* need : {"/n1", "/n2", "/n3", "/n4", "/n5"})
				shape.needs.push_back(*SharedMemoryName::parse(need));
			break;
		case Broken::twinNeeds:
			shape.needs.push_back(*SharedMemoryName::parse("/x"));
			shape.needs.push_back(*SharedMemoryName::parse("/x"));
			break;
		}
		cx.loop(reports, std::make_unique<Reports>());
		cx.program(shape, reports, &Reports::onFeed);
		if (behaviour.broken == Broken::secondProgram) cx.program(ProgramShape{.executable = player});
		cx.use(Copy{});
		return Configured::applied();
	}
};

using Harness = testing::TypeHarness<spec, Piped>;

std::unique_ptr<Harness> fresh(Harness::Options options = {}, Behaviour asked = {}) {
	fed.clear();
	behaviour = asked;
	auto harness = std::make_unique<Harness>(std::move(options));
	REQUIRE_FALSE(harness->parse({}));
	return harness;
}

/// Added, its block and pipe receiver ready: its program launched (and, unless held, started and the node running).
std::unique_ptr<Harness> launched(Harness::Options options = {}, Behaviour asked = {}) {
	auto h = fresh(std::move(options), std::move(asked));
	h->add();
	h->blockReady();
	h->node().ready(fromPipe.name);
	REQUIRE(h->programs().size() == 1);
	return h;
}

std::string failure(const Harness& h) {
	const NodeHealth health = h.health();
	return health.reason ? std::string(failureCode(health.reason->code)) + ": " + health.reason->detail : "";
}

std::size_t diagnosed(const Harness& h, std::string_view code) {
	std::size_t count = 0;
	for (const auto& seen : const_cast<Harness&>(h)->diagnostics()) count += seen.code == code ? 1 : 0;
	return count;
}

} // namespace program_test

using namespace program_test;

TEST_CASE(
	"A pipe receiver's delay is the pipe module's 8192 frames at the writer's rate, in graph frames rounded half up",
	"[program][module_run]") {
	const FrameClock graph(SampleRate::fixed<48000>());
	const auto at = [&](SampleRate rate) { return moduleRunDelay(PipeReceiverShape{PipeFormat::s16le, rate}, graph); };
	CHECK(at(SampleRate::fixed<44100>()) == Delay{Latency{8916, 8916}});
	CHECK(at(SampleRate::fixed<48000>()) == Delay{Latency{8192, 8192}});
	CHECK(at(SampleRate::fixed<22050>()) == Delay{Latency{17833, 17833}});
	STATIC_REQUIRE(ModuleRunShape<PipeReceiverShape>);
	CHECK(claimsOf(PipeReceiverShape{PipeFormat::s32le, SampleRate::fixed<48000>()}).empty());
}

TEST_CASE("The pipe receiver is an engine kind, pipe-receiver on the wire, ready within 5 s, and a program's wait is "
          "program",
          "[program]") {
	CHECK(companionKindName(CompanionKind::pipeReceiver) == "pipe-receiver");
	CHECK(isEngineKind(CompanionKind::pipeReceiver));
	CHECK(readinessBound(CompanionKind::pipeReceiver) == pipeReceiverReadinessBound);
	CHECK(pipeReceiverReadinessBound == 5s);
	CHECK(waitingKindName(WaitingFor::Kind::program) == "program");
}

TEST_CASE("A program's words: its runtime paths, its end, its arguments and its name", "[program]") {
	CHECK(runtimePath("/r", "kitchen", 7, "feed") == "/r/kitchen.7.feed");
	CHECK(programExitDetail("x", ProgramExit{ProgramExit::How::exited, 3, {}}) == "x exited with status 3");
	CHECK(programExitDetail("x", ProgramExit{ProgramExit::How::exited, 1, "no nqptp"}) ==
	      "x exited with status 1: no nqptp");
	CHECK(programExitDetail("x", ProgramExit{ProgramExit::How::killed, 9, {}}) == "x was killed by signal 9 (SIGKILL)");
	CHECK(programExitDetail("x", ProgramExit{ProgramExit::How::killed, 15, "bye"}) ==
	      "x was killed by signal 15 (SIGTERM): bye");
	CHECK(ProgramArg("a").parts == std::vector<ProgramPart>{part::Text{"a"}});
	CHECK(ProgramArg(std::string("b")).parts == std::vector<ProgramPart>{part::Text{"b"}});
	CHECK(ProgramArg(ProgramPart{part::Feed{}}).parts == std::vector<ProgramPart>{part::Feed{}});
	CHECK(ProgramArg({part::Text{"--fifo="}, part::Fifo{"in"}}).parts ==
	      std::vector<ProgramPart>{part::Text{"--fifo="}, part::Fifo{"in"}});
	CHECK(ProgramPath{"/usr/local/bin/x"}.name() == "x");
	CHECK(ProgramPath{"/usr/local/bin/x"}.path() == "/usr/local/bin/x");
}

TEST_CASE("A shared-memory name is \"/\" then 1 to 62 of [A-Za-z0-9._-], never \"/.\" or \"/..\"", "[program][needs]") {
	CHECK(SharedMemoryName::parse("/ear-kitchen")->text() == "/ear-kitchen");
	const std::string longest = "/" + std::string(62, 'a');
	REQUIRE(longest.size() == 63);
	CHECK(SharedMemoryName::parse(longest)->text() == longest);
	CHECK(SharedMemoryName::parse("/A.b_c-9"));
	for (const std::string& bad :
	     {std::string(), std::string("/"), std::string("ear"), std::string("/a/b"), std::string("/."),
	      std::string("/.."), std::string("/a b"), "/" + std::string(63, 'a')}) {
		INFO(bad);
		CHECK_FALSE(SharedMemoryName::parse(bad));
	}
}

TEST_CASE("A program ended for a removed name says so, and its deadline names what it needs", "[program][needs]") {
	CHECK(programExitDetail("x", ProgramExit{ProgramExit::How::killed, 15, "bye", "/ear-kitchen"}) ==
	      "x was ended: shared memory /ear-kitchen, which it needs, was removed");
	CHECK(programExitDetail("x", ProgramExit{ProgramExit::How::exited, 0, {}, "/a"}) ==
	      "x was ended: shared memory /a, which it needs, was removed");
	CHECK(programOverdueDetail("p", {}) == "p did not start within 5 s");
	const std::vector<SharedMemoryName> one{*SharedMemoryName::parse("/x")};
	CHECK(programOverdueDetail("p", one) == "p did not start, or find /x, within 5 s");
	const std::vector<SharedMemoryName> two{*SharedMemoryName::parse("/x"), *SharedMemoryName::parse("/y")};
	CHECK(programOverdueDetail("p", two) == "p did not start, or find /x, /y, within 5 s");
}

TEST_CASE("A program that needs shared memory is launched with the names, and its node waits for it as program after "
          "it started, running only once it is ready",
          "[program][needs]") {
	auto h = launched({.holdPrograms = true}, {.needs = {"/x", "/ear-kitchen"}});
	CHECK(h->programs().at(0).launch.needs == std::vector<std::string>{"/x", "/ear-kitchen"});
	h->programStarted();
	CHECK(h->programs().at(0).started);
	CHECK_FALSE(h->programs().at(0).ready);
	CHECK(h->phase() == "configuring");
	h->node().advance(2s); // waitingFor is told only once a wait has lasted waitingForDelay
	CHECK(h->health().waitingFor == std::vector<WaitingFor>{{WaitingFor::Kind::program, "fake-player"}});
	h->programReady();
	CHECK(h->phase() == "running");
	CHECK_THROWS_AS(h->programReady(), std::logic_error); // told once
}

TEST_CASE("A program that needs nothing is ready with its start, held or not", "[program][needs]") {
	auto held = launched({.holdPrograms = true});
	CHECK_THROWS_AS(held->programReady(), std::logic_error); // not started
	held->programStarted();
	CHECK(held->programs().at(0).ready);
	CHECK(held->phase() == "running");
	auto unheld = launched({}, {.needs = {"/x"}});
	CHECK(unheld->programs().at(0).ready);
	CHECK(unheld->phase() == "running");
}

TEST_CASE("A program that ends after it started and before it was ready fails its node start-failed with its words",
          "[program][needs]") {
	auto h = launched({.holdPrograms = true}, {.needs = {"/x"}});
	h->programStarted();
	h->programExited(0, ProgramExit{ProgramExit::How::exited, 1, "The NQPTP service can not be found."});
	CHECK(failure(*h) == "start-failed: fake-player exited with status 1: The NQPTP service can not be found.");
}

TEST_CASE("A program that needs shared memory and is not ready within 5 s fails its node start-failed, naming what it "
          "needs",
          "[program][needs][deadline]") {
	auto h = launched({.holdPrograms = true}, {.needs = {"/x"}});
	h->programStarted();
	h->node().advance(programReadinessBound - 1ms);
	CHECK(h->phase() == "configuring");
	h->node().advance(1ms);
	CHECK(failure(*h) == "start-failed: fake-player did not start, or find /x, within 5 s");
}

TEST_CASE("NodeHost resolves every part of the program it launches: the granted port, the pipe receiver's FIFO, the "
          "feed and the files, each a runtime path of the configuration",
          "[program]") {
	auto h = launched();
	const ProgramLaunch& launch = h->programs().at(0).launch;
	CHECK(launch.node == "node");
	CHECK(launch.executable == "/usr/local/bin/fake-player");
	CHECK(launch.argv ==
	      std::vector<std::string>{"fake-player", "--port", "49152", "--config=/ear-runtime/node.1.config"});
	REQUIRE(launch.files.size() == 1);
	CHECK(launch.files[0].first == runtimePath("/ear-runtime", "node", 1, "config"));
	CHECK(launch.files[0].second == "pipe = /ear-runtime/node.1.from_pipe.fifo\nfeed = /ear-runtime/node.1.feed\n");
	CHECK(launch.feed == runtimePath("/ear-runtime", "node", 1, "feed"));
	CHECK(launch.folder == "/ear-runtime");
	CHECK(h->phase() == "running");
}

TEST_CASE("A program starts last: it is launched only once the pipe receiver is ready, the node waits for it as "
          "program, and runs once it started",
          "[program]") {
	auto h = fresh({.holdPrograms = true});
	h->add();
	h->blockReady();
	CHECK(h->programs().empty()); // the pipe receiver is not ready yet
	h->node().advance(2s);
	CHECK(h->programs().empty());
	h->node().ready(fromPipe.name);
	REQUIRE(h->programs().size() == 1);
	CHECK(h->phase() == "configuring");
	h->node().advance(2s);
	CHECK(h->health().waitingFor == std::vector<WaitingFor>{{WaitingFor::Kind::program, "fake-player"}});
	h->programStarted();
	CHECK(h->phase() == "running");
}

TEST_CASE("A program not started within 5 s fails its node start-failed", "[program][deadline]") {
	auto h = launched({.holdPrograms = true});
	h->node().advance(programReadinessBound - 1ms);
	CHECK(h->phase() == "configuring");
	h->node().advance(1ms);
	CHECK(failure(*h) == "start-failed: fake-player did not start within 5 s");
	CHECK(h->programs().at(0).stopped); // the teardown stops what was launched
}

TEST_CASE("The report feed reaches the state's member on its loop, in order, once the node is configured",
          "[program]") {
	auto h = launched();
	h->runLoops();
	h->feedProgram(0, "first ");
	h->feedProgram(0, "second");
	CHECK(fed.empty()); // on the loop, not the main loop
	h->runLoops();
	CHECK(fed == "first second");
}

TEST_CASE("A program's lines are the node's log entries, its name their source and stdout or stderr their topic",
          "[program]") {
	auto h = launched();
	h->programSays(0, LogLevel::info, "listening");
	h->programSays(0, LogLevel::warn, "no sender");
	std::vector<LogEntry> lines;
	for (const LogEntry& entry : h->node().logs())
		if (entry.source == "fake-player") lines.push_back(entry);
	REQUIRE(lines.size() == 2);
	CHECK(lines[0].text == "listening");
	CHECK(lines[0].topic == "stdout");
	CHECK(lines[0].level == LogLevel::info);
	CHECK(lines[0].node == std::optional<NodeName>("node"));
	CHECK(lines[1].topic == "stderr");
	CHECK(lines[1].level == LogLevel::warn);
}

TEST_CASE("A program's end fails its node: start-failed before it first rested, lost after, and lost while stopped",
          "[program]") {
	const ProgramExit exit{ProgramExit::How::exited, 1, "The NQPTP service can not be found."};
	const std::string words = "fake-player exited with status 1: The NQPTP service can not be found.";
	SECTION("before it rested") {
		auto h = launched({.autoComplete = false, .holdPrograms = true});
		h->programStarted();
		REQUIRE(h->phase() == "starting"); // its drive in flight
		h->programExited(0, exit);
		CHECK(failure(*h) == "start-failed: " + words);
	}
	SECTION("running") {
		auto h = launched();
		h->programExited(0, exit);
		CHECK(failure(*h) == "lost: " + words);
	}
	SECTION("stopped") {
		auto h = launched();
		REQUIRE(h->stop().has_value());
		REQUIRE(h->phase() == "stopped");
		h->programExited(0, exit);
		CHECK(failure(*h) == "lost: " + words);
	}
}

TEST_CASE("A program that cannot start fails its node start-failed, naming the program and why", "[program]") {
	auto h = launched({.holdPrograms = true});
	h->programRefused(0, "No such file or directory");
	CHECK(failure(*h) == "start-failed: cannot start fake-player: No such file or directory");
}

TEST_CASE("A stop and a start keep the program, and a restart stops it, waits for its end, frees it, and launches the "
          "next with the new configuration's paths",
          "[program][teardown]") {
	auto h = launched({.holdPrograms = true});
	h->programStarted();
	REQUIRE(h->stop().has_value());
	REQUIRE(h->start().has_value());
	CHECK(h->phase() == "running");
	CHECK(h->programs().size() == 1);
	CHECK_FALSE(h->programs().at(0).stopped);

	REQUIRE(h->restart().has_value());
	CHECK(h->programs().at(0).stopped);
	CHECK_FALSE(h->programs().at(0).destroyed); // the teardown waits for its end
	CHECK(h->programs().size() == 1);
	h->programExited(0, ProgramExit{ProgramExit::How::killed, 15, {}});
	CHECK(h->programs().at(0).destroyed);
	CHECK(failure(*h).empty()); // the end a teardown waits for is no failure
	h->node().ready(fromPipe.name);
	REQUIRE(h->programs().size() == 2);
	const ProgramLaunch& next = h->programs().at(1).launch;
	CHECK(next.argv.at(2) == "49152"); // its port released once the first program was reaped
	CHECK(next.files.at(0).first == "/ear-runtime/node.2.config");
	CHECK(next.feed == "/ear-runtime/node.2.feed");
	CHECK(next.files.at(0).second.find("/ear-runtime/node.2.from_pipe.fifo") != std::string::npos);
}

TEST_CASE("A program never reaped leaks its configuration at the close deadline, and its late end closes it late",
          "[program][teardown]") {
	auto h = launched({.holdPrograms = true});
	h->programStarted();
	REQUIRE(h->restart().has_value());
	h->node().advance(defaultCloseDeadline);
	REQUIRE(diagnosed(*h, "ear.close-overdue") == 1);
	bool counted = false;
	for (const auto& seen : h->node().diagnostics())
		if (seen.code == "ear.close-overdue") counted = seen.detail.find("1 programs open") != std::string::npos;
	CHECK(counted);
	CHECK(diagnosed(*h, "ear.close-late") == 0);
	h->programExited(0, ProgramExit{ProgramExit::How::killed, 9, {}});
	CHECK(diagnosed(*h, "ear.close-late") == 1);
}

TEST_CASE("Every rule a program's shape breaks fails its node with the rule's words", "[program][rules]") {
	struct Row {
		Broken broken;
		std::string words;
	};
	for (const Row& row : {
			 Row{Broken::secondProgram, "configure threw: a second program: one per configuration"},
			 Row{Broken::badFileName, "configure threw: program fake-player: bad file name Bad_Name"},
			 Row{Broken::feedFileName, "configure threw: program fake-player: bad file name feed"},
			 Row{Broken::twinFileName, "configure threw: program fake-player: bad file name config"},
			 Row{Broken::tooManyFiles, "configure threw: program fake-player: more than 8 files"},
			 Row{Broken::feedWithoutFeed, "configure threw: program fake-player names its feed but has none"},
			 Row{Broken::unknownFile, "configure threw: program fake-player names no file missing"},
			 Row{Broken::foreignPort,
	             "configure threw: program fake-player names a port this configuration was not granted"},
			 Row{Broken::tooManyNeeds, "configure threw: program fake-player: more than 4 needs"},
			 Row{Broken::twinNeeds, "configure threw: program fake-player needs /x twice"},
		 }) {
		INFO(row.words);
		auto h = fresh({}, {.broken = row.broken});
		h->add();
		CHECK(failure(*h) == "start-failed: " + row.words);
		CHECK(h->programs().empty());
	}
	auto h = fresh({}, {.broken = Broken::fifoWithoutReceiver, .pipe = false});
	h->add();
	CHECK(failure(*h) ==
	      "start-failed: configure gave program fake-player the FIFO of bank from_pipe, which has no pipe receiver");
}

TEST_CASE("A host with no runtime folder refuses a pipe receiver and a program at the call", "[program][rules]") {
	SECTION("a pipe receiver") {
		auto h = fresh({.runtimeFolder = ""});
		h->add();
		CHECK(failure(*h) == "start-failed: configure threw: no runtime folder in this host");
	}
	SECTION("a program") {
		auto h = fresh({.runtimeFolder = ""}, {.pipe = false});
		h->add();
		CHECK(failure(*h) == "start-failed: configure threw: no runtime folder in this host");
	}
}

TEST_CASE("A pipe receiver is <node>.pipe, kind pipe-receiver, reading its configuration's FIFO, its module's buffer "
          "beyond its bank, and engine-refused past 5 s",
          "[program][companions]") {
	auto h = launched();
	const testing::CompanionFake& made = h->node().companion(fromPipe.name);
	CHECK(made.kind == CompanionKind::pipeReceiver);
	CHECK(made.name == "node.pipe");
	CHECK(made.fifo == "/ear-runtime/node.1.from_pipe.fifo");
	REQUIRE(h->companionList().size() == 1);
	CHECK(h->companionList()[0].kind == CompanionKind::pipeReceiver);
	CHECK(h->companionList()[0].name == "node.pipe");
	const LatencyPlan& plan = h->node().latencyPlan();
	REQUIRE(plan.beyond.size() == 1);
	CHECK(plan.beyond[0].first == "from_pipe");
	CHECK(plan.beyond[0].second == moduleRunDelay(PipeReceiverShape{PipeFormat::s16le, SampleRate::fixed<44100>()},
	                                              FrameClock(SampleRate::fixed<48000>())));

	auto late = fresh();
	late->add();
	late->blockReady();
	late->node().advance(pipeReceiverReadinessBound);
	CHECK(failure(*late) == "engine-refused: the pipe receiver never ran");
}
