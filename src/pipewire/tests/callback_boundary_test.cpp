// The engine's callback boundary (docs/architecture.md#the-callback-boundary): a guarded callback catches what its
// member threw and reports it to the installed Faults, naming the member and where the main loop was when it was thrown,
// then tells its FaultOwner; a realtime one calls its member and adds nothing; the main-loop executor runs each posted
// item and each timer through it, so one that throws never drops the rest; and with no Faults installed the process
// ends, saying first which callback threw. Every wait runs the main loop until the awaited work has run; the deadline
// only fails the test.
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <pipewire/pipewire.h>

#include "../callback_boundary.hpp"
#include "catch2/catch.hpp"
#include "ear/core/main_loop_activity.hpp"
#include "ear/core/node_host.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/faults.hpp"
#include "ear/pipewire/main_loop_executor.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;

/// What the boundary reported, in order, with whatever an owner was told among it.
struct Report {
	std::string callback;
	std::string where;
	std::string detail;
	bool owned = false;
	bool toOwner = false; // a FaultOwner's faulted(), not the Faults
	std::string node{};   // the owner's node, as the Faults was told it
};

/// Records instead of ending the process (main.cpp ends it for a fault no adapter owns), so a test sees what the
/// boundary told it and what ran after.
class RecordingFaults final : public Faults {
public:
	void caught(std::string_view callback, std::string_view where, std::string_view detail, std::string_view node,
	            bool owned) noexcept override {
		reports.push_back(
			Report{std::string(callback), std::string(where), std::string(detail), owned, false, std::string(node)});
	}
	std::vector<Report> reports;
};

/// Installs `faults` for one test made without an engine, and takes it away after.
class Installed {
public:
	explicit Installed(Faults& faults) {
		if (!boundary::install(&faults)) FAIL("the boundary is held: an engine or another test did not let go");
	}
	~Installed() { boundary::uninstall(); }
	Installed(const Installed&) = delete;
	Installed& operator=(const Installed&) = delete;
};

struct Thrower {
	void onEvent(std::uint64_t count) {
		if (count == 0) throw std::runtime_error("boom");
		++ran;
	}
	void onOdd(std::uint64_t /*count*/) { throw 7; }
	int onInvoke() { throw std::logic_error("invoked"); }
	int onRealtime() noexcept { return 42; }
	/// Throws from inside a hook's scope opened inside a flush item's, as NodeHost's calls are.
	void onNested(std::uint64_t /*count*/) {
		const MainLoopActivity::Scope item("make block", "hall");
		const MainLoopActivity::Scope hook("configure", "hall");
		throw std::runtime_error("configure threw");
	}
	int ran = 0;
};

/// A destructor that PipeWire calls back from (as pw_filter_destroy tells a state change), run by an unwinding.
struct CallsBackWhenDestroyed {
	~CallsBackWhenDestroyed() { Guarded<&Thrower::onEvent>::call(&thrower, 1); }
	Thrower thrower;
};

class OwnedThrower final : public FaultOwner {
public:
	explicit OwnedThrower(RecordingFaults& faults) : faults_(faults) {}
	std::string_view faultNode() const noexcept override { return "hall"; }
	void faulted(std::string_view callback, std::string_view detail) override {
		faults_.reports.push_back(Report{std::string(callback), {}, std::string(detail), true, true});
		if (throwWhenTold) throw std::logic_error("told twice"); // as NodeHost with a bug of its own, told
	}
	void onEvent(std::uint64_t /*count*/) { throw std::runtime_error("owned boom"); }
	/// Throws out of NodeHost's scope, as a throw NodeHost let out through an adapter's event would.
	void onThroughHost(std::uint64_t /*count*/) {
		const MainLoopActivity::Scope scope("start", "porch");
		throw std::logic_error("NodeHost's bug");
	}
	bool throwWhenTold = false;

private:
	RecordingFaults& faults_;
};

std::unique_ptr<Engine> makeEngine(Faults* faults) {
	for (const char* name : {"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE", "DBUS_SESSION_BUS_ADDRESS",
	                         "DBUS_SYSTEM_BUS_ADDRESS"}) {
		unsetenv(name);
	}
	auto engine = Engine::create(EngineSettings{EAR_ENGINE_CONF, ear::SampleRate::fixed<48000>(), 256, faults});
	if (!engine) FAIL("Engine::create: " << engine.error().detail);
	return std::move(*engine);
}

/// Runs the main loop until something calls engine.quit(); the deadline only fails the test. Its timer is registered
/// through the boundary too.
class Deadline {
public:
	explicit Deadline(Engine& engine) : engine_(engine) {}
	bool run(std::chrono::seconds deadline = std::chrono::seconds(5)) {
		LoopSource timer;
		addTimer(timer, engine_.mainLoop(), guarded<&Deadline::onTimeout>, this);
		timespec value{static_cast<time_t>(deadline.count()), 0};
		pw_loop_update_timer(engine_.mainLoop(), timer.get(), &value, nullptr, false);
		engine_.run();
		return !timedOut_;
	}

private:
	void onTimeout(std::uint64_t /*expirations*/) {
		timedOut_ = true;
		engine_.quit();
	}
	Engine& engine_;
	bool timedOut_ = false;
};

} // namespace

TEST_CASE("A guarded callback that throws is reported naming its member and what it said, then returns", "[boundary]") {
	RecordingFaults faults;
	const Installed installed(faults);
	Thrower thrower;
	Guarded<&Thrower::onEvent>::call(&thrower, 0);
	Guarded<&Thrower::onEvent>::call(&thrower, 1); // the boundary returned: the next call runs
	REQUIRE(faults.reports.size() == 1);
	CHECK(faults.reports[0].callback == "Thrower::onEvent");
	CHECK(faults.reports[0].detail == "boom");
	CHECK(faults.reports[0].where.empty());
	CHECK_FALSE(faults.reports[0].owned);
	CHECK(thrower.ran == 1);

	Guarded<&Thrower::onOdd>::call(&thrower, 0);
	REQUIRE(faults.reports.size() == 2);
	CHECK(faults.reports[1].callback == "Thrower::onOdd");
	CHECK(faults.reports[1].detail == "an exception that is no std::exception");

	CHECK(Guarded<&Thrower::onInvoke>::invoked(nullptr, false, 0, nullptr, 0, &thrower) == 0);
	REQUIRE(faults.reports.size() == 3);
	CHECK(faults.reports[2].callback == "Thrower::onInvoke");
	CHECK(faults.reports[2].detail == "invoked");
}

TEST_CASE("A fault names the innermost call the throw left on the main loop, and only that throw", "[boundary]") {
	RecordingFaults faults;
	const Installed installed(faults);
	MainLoopActivity::clear();
	Thrower thrower;
	Guarded<&Thrower::onNested>::call(&thrower, 0);
	REQUIRE(faults.reports.size() == 1);
	CHECK(faults.reports[0].where == "configure hall");
	CHECK(MainLoopActivity::current().empty());

	// A throw handled inside a callback leaves its place, which the next callback forgets.
	try {
		const MainLoopActivity::Scope scope("start", "porch");
		throw std::runtime_error("handled");
	} catch (const std::runtime_error&) {
	}
	Guarded<&Thrower::onEvent>::call(&thrower, 0);
	REQUIRE(faults.reports.size() == 2);
	CHECK(faults.reports[1].where.empty());

	// A callback PipeWire makes inside a call that is still open is placed in that call.
	{
		const MainLoopActivity::Scope scope("destroy block", "desk");
		Guarded<&Thrower::onEvent>::call(&thrower, 0);
	}
	REQUIRE(faults.reports.size() == 3);
	CHECK(faults.reports[2].where == "destroy block desk");
}

TEST_CASE("A rethrown exception keeps the place it was first thrown from", "[boundary]") {
	MainLoopActivity::clear();
	MainLoopActivity::Saved at;
	std::exception_ptr kept;
	try {
		const MainLoopActivity::Scope scope("recipe", "attic");
		throw std::runtime_error("recipe threw");
	} catch (...) {
		kept = std::current_exception();
		at = *MainLoopActivity::takeThrown();
	}
	CHECK_FALSE(MainLoopActivity::takeThrown());
	try {
		const MainLoopActivity::Scope later("start", "cellar"); // a later call, run before the rethrow
		MainLoopActivity::putThrown(at);
		std::rethrow_exception(kept); // ends `later`, which must not replace the place put back
	} catch (...) {
		const auto place = MainLoopActivity::takeThrown();
		REQUIRE(place);
		CHECK(MainLoopActivity::describe(*place) == "recipe attic");
	}
}

TEST_CASE("A callback made while an exception unwinds leaves the place that exception carries", "[boundary]") {
	RecordingFaults faults;
	const Installed installed(faults);
	MainLoopActivity::clear();
	try {
		const CallsBackWhenDestroyed destroyed;
		const MainLoopActivity::Scope scope("destroy block", "hall");
		throw std::runtime_error("destroy threw");
	} catch (const std::runtime_error&) {
		const auto at = MainLoopActivity::takeThrown();
		REQUIRE(at);
		CHECK(MainLoopActivity::describe(*at) == "destroy block hall");
	}
	CHECK(faults.reports.empty());
}

TEST_CASE("A second holder of the boundary is refused while one holds it", "[boundary]") {
	RecordingFaults faults;
	const Installed installed(faults);
	RecordingFaults other;
	CHECK_FALSE(boundary::install(&other));
	auto engine = Engine::create(EngineSettings{EAR_ENGINE_CONF, ear::SampleRate::fixed<48000>(), 256, &other});
	REQUIRE_FALSE(engine);
	CHECK(engine.error().detail == "another engine is running in this process: ear runs one at a time");
	Thrower thrower;
	Guarded<&Thrower::onEvent>::call(&thrower, 0); // still reported to the first holder's
	CHECK(faults.reports.size() == 1);
	CHECK(other.reports.empty());
}

TEST_CASE("A FaultOwner's callback that throws is reported with its node, then the owner is told", "[boundary]") {
	RecordingFaults faults;
	const Installed installed(faults);
	MainLoopActivity::clear();
	OwnedThrower owner(faults);
	Guarded<&OwnedThrower::onEvent>::call(&owner, 0);
	REQUIRE(faults.reports.size() == 2);
	CHECK(faults.reports[0].callback == "OwnedThrower::onEvent");
	CHECK(faults.reports[0].owned);
	CHECK(faults.reports[0].node == "hall");
	CHECK_FALSE(faults.reports[0].toOwner);
	CHECK(faults.reports[1].toOwner);
	CHECK(faults.reports[1].callback == "OwnedThrower::onEvent");
	CHECK(faults.reports[1].detail == "owned boom");
}

// docs/architecture.md#faults-by-owner: only NodeHost leaves a place with a throw, so one that carries a place came out
// of NodeHost, through whichever adapter's event it was told by: NodeHost's bug, never the adapter's.
TEST_CASE("A throw that came out of NodeHost through an owner's callback has no owner, and the owner is not told",
          "[boundary]") {
	RecordingFaults faults;
	const Installed installed(faults);
	MainLoopActivity::clear();
	OwnedThrower owner(faults);
	Guarded<&OwnedThrower::onThroughHost>::call(&owner, 0);
	REQUIRE(faults.reports.size() == 1);
	CHECK(faults.reports[0].callback == "OwnedThrower::onThroughHost");
	CHECK(faults.reports[0].where == "start porch");
	CHECK(faults.reports[0].detail == "NodeHost's bug");
	CHECK_FALSE(faults.reports[0].owned);
	CHECK(faults.reports[0].node.empty());
}

TEST_CASE("An owner that throws as it is told is reported again, with no owner", "[boundary]") {
	RecordingFaults faults;
	const Installed installed(faults);
	MainLoopActivity::clear();
	OwnedThrower owner(faults);
	owner.throwWhenTold = true;
	Guarded<&OwnedThrower::onEvent>::call(&owner, 0);
	REQUIRE(faults.reports.size() == 3);
	CHECK(faults.reports[0].owned);
	CHECK(faults.reports[1].toOwner);
	CHECK(faults.reports[2].callback == "OwnedThrower::onEvent");
	CHECK(faults.reports[2].detail == "told twice");
	CHECK_FALSE(faults.reports[2].owned);
	CHECK_FALSE(faults.reports[2].toOwner);
}

TEST_CASE("A realtime callback calls its member and nothing else", "[boundary]") {
	Thrower thrower;
	CHECK(Realtime<&Thrower::onRealtime>::invoked(nullptr, false, 0, nullptr, 0, &thrower) == 42);
	CHECK(Realtime<&Thrower::onRealtime>::name == "Thrower::onRealtime");
}

TEST_CASE("The main-loop executor runs every item of a batch when one throws, and reports the one", "[boundary]") {
	RecordingFaults faults;
	auto engine = makeEngine(&faults);
	{
		MainLoopExecutor executor(*engine);
		std::vector<int> ran;
		executor.post([&] { ran.push_back(1); });
		executor.post([&] {
			ran.push_back(2);
			throw std::runtime_error("item two");
		});
		executor.post([&] {
			ran.push_back(3);
			engine->quit();
		});
		Deadline deadline(*engine);
		REQUIRE(deadline.run());
		CHECK(ran == std::vector<int>{1, 2, 3});
		REQUIRE(faults.reports.size() == 1);
		CHECK(faults.reports[0].callback == "MainLoopExecutor's posted work");
		CHECK(faults.reports[0].detail == "item two");
	}
	engine.reset();
}

TEST_CASE("A main-loop timer that throws is reported as the executor's timer, and the loop runs on", "[boundary]") {
	RecordingFaults faults;
	auto engine = makeEngine(&faults);
	{
		MainLoopExecutor executor(*engine);
		const auto threw = executor.after(std::chrono::milliseconds(1), [] { throw std::runtime_error("timer"); });
		bool later = false;
		const auto after = executor.after(std::chrono::milliseconds(20), [&] {
			later = true;
			engine->quit();
		});
		Deadline deadline(*engine);
		REQUIRE(deadline.run());
		CHECK(later);
		REQUIRE(faults.reports.size() == 1);
		CHECK(faults.reports[0].callback == "LoopTimer::onTimeout");
		CHECK(faults.reports[0].detail == "timer");
	}
	engine.reset();
}

TEST_CASE("With no Faults installed a callback that throws ends the process, saying first which one threw",
          "[boundary][crash]") {
	int ends[2];
	REQUIRE(::pipe2(ends, O_CLOEXEC) == 0);
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		const rlimit noCore{0, 0}; // a crash on purpose leaves no core file behind
		::setrlimit(RLIMIT_CORE, &noCore);
		::dup2(ends[1], STDOUT_FILENO);
		::dup2(ends[1], STDERR_FILENO);
		boundary::install(nullptr);
		Thrower thrower;
		Guarded<&Thrower::onEvent>::call(&thrower, 0);
		::_exit(91); // never: the boundary ended the process
	}
	::close(ends[1]);
	std::string written;
	char buffer[4096];
	ssize_t got;
	while ((got = ::read(ends[0], buffer, sizeof buffer)) > 0) written.append(buffer, static_cast<std::size_t>(got));
	::close(ends[0]);
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	INFO(written);
	REQUIRE(WIFSIGNALED(status));
	CHECK(WTERMSIG(status) == SIGABRT);
	const auto line = written.find("ear: internal error in Thrower::onEvent: boom\n");
	const auto terminate = written.find("terminate called after throwing an instance of 'std::runtime_error'");
	REQUIRE(line != std::string::npos);
	REQUIRE(terminate != std::string::npos);
	CHECK(line < terminate);
}

namespace {

/// What a forked child wrote to its standard output and error, and how it ended.
struct ChildEnd {
	std::string written;
	int status = 0;
};

/// Runs `body` in a forked child whose standard output and error are a pipe this reads; `body` must end the process.
template <class Body> ChildEnd inChild(Body body) {
	int ends[2];
	REQUIRE(::pipe2(ends, O_CLOEXEC) == 0);
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		const rlimit noCore{0, 0}; // a crash on purpose leaves no core file behind
		::setrlimit(RLIMIT_CORE, &noCore);
		::dup2(ends[1], STDOUT_FILENO);
		::dup2(ends[1], STDERR_FILENO);
		body();
		::_exit(91); // never: the boundary ended the process
	}
	::close(ends[1]);
	ChildEnd end;
	char buffer[4096];
	ssize_t got;
	while ((got = ::read(ends[0], buffer, sizeof buffer)) > 0)
		end.written.append(buffer, static_cast<std::size_t>(got));
	::close(ends[0]);
	REQUIRE(::waitpid(child, &end.status, 0) == child);
	return end;
}

/// The child's main loop: what is posted runs when the test says; no timer ever fires.
class QueueLoop final : public Executor, public Timers {
public:
	void post(std::move_only_function<void()> work) override { queue.push_back(std::move(work)); }
	std::unique_ptr<Timer> after(std::chrono::milliseconds, std::move_only_function<void()>) override {
		return std::make_unique<Timer>();
	}
	std::chrono::steady_clock::time_point now() const override { return {}; }
	void runPosted() {
		while (!queue.empty()) {
			auto next = std::move(queue.front());
			queue.pop_front();
			next();
		}
	}
	std::deque<std::move_only_function<void()>> queue;
};

/// A block that tells its events from a guarded callback of its own, as FilterPortBlock does, and is a FaultOwner as
/// it is; its carryLatency throws, as a block that disagrees with NodeHost's own plan check would.
class TellingBlock final : public PortBlock, public FaultOwner {
public:
	explicit TellingBlock(PortBlockEvents& events) : events_(events) {}
	void carryLatency(const LatencyPlan&) override { throw std::logic_error("a plan the block cannot carry"); }
	void drive(DriveRequest, std::move_only_function<void(std::uint64_t)>) override {}
	void fadeOut(std::move_only_function<void()>) override {}
	void settle() override {}
	std::uint64_t takenAt() const noexcept override { return 0; }
	std::uint32_t blockId() const noexcept override { return 0xFFFFFFFFU; }
	NodeCounts counts() const noexcept override { return {}; }
	std::string_view faultNode() const noexcept override { return "amp"; }
	void faulted(std::string_view, std::string_view) override { std::fputs("the block was told\n", stderr); }
	void onStreaming(std::uint64_t /*count*/) { events_.ready(); }

private:
	PortBlockEvents& events_;
};

struct TellingBlocks final : PortBlockFactory {
	TellingBlock* made = nullptr;
	std::unique_ptr<PortBlock> make(const NodeName&, const Ports&, const AudioFormat&,
	                                PortBlockEvents& events) override {
		auto block = std::make_unique<TellingBlock>(events);
		made = block.get();
		return block;
	}
};

struct NoCards final : SoundCardCompanions {
	std::unique_ptr<EngineCompanion> make(const CompanionTarget&, const SoundCardShape&,
	                                      EngineCompanionEvents&) override {
		return nullptr;
	}
};
struct NoReceivers final : RtpReceiverCompanions {
	std::unique_ptr<EngineCompanion> make(const CompanionTarget&, const RtpReceiverShape&,
	                                      EngineCompanionEvents&) override {
		return nullptr;
	}
};
struct NoHandoffs final : HandoffAdapter {
	std::unique_ptr<Handoff> make(const NodeName&, std::uint32_t, std::uint32_t, HandoffConsumer&) override {
		return nullptr;
	}
};
struct NoSinks final : SinkAdapter {
	std::unique_ptr<Sink> make(const NodeName&, std::string_view, std::uint32_t, std::uint32_t, SampleRate) override {
		return nullptr;
	}
};
struct NoSources final : SourceAdapter {
	std::unique_ptr<Source> make(const NodeName&, std::string_view, std::uint32_t, std::uint32_t, SampleRate,
	                             std::unique_ptr<SourceClock>) override {
		return nullptr;
	}
};

class SilentCode final : public AudioCode {
public:
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>>, std::uint32_t,
	             const GraphCycle&) noexcept override {}
	void release() noexcept override {}
};

class SilentConfig final : public NodeConfig {
public:
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext&) const override { return std::make_unique<SilentCode>(); }

private:
	Ports ports_{{{Direction::output, "out", {"mono"}}}};
};

} // namespace

// docs/architecture.md#faults-by-owner: a throw of NodeHost's own is NodeHost's bug, whose state is every node's, so it
// ends the process through the boundary, even out of the callback of an adapter that owns its faults, which is never
// told.
TEST_CASE("A std::logic_error of NodeHost's own, out of a block's callback, ends the process with the boundary's line, "
          "the block never told",
          "[boundary][crash]") {
	const ChildEnd end = inChild([] {
		// The Faults ear runs with: it returns for a fault an adapter owns, so the child would run on to _exit(91)
		// had the block been taken for the owner.
		static RecordThenEnd faults(
			[](const InternalError& error) {
				std::fprintf(stderr, "record %s fatal=%d node=%s\n", std::string(error.callback).c_str(),
			                 error.fatal ? 1 : 0, std::string(error.node).c_str());
			},
			[](std::string_view line) { std::fwrite(line.data(), 1, line.size(), stderr); });
		boundary::install(&faults);
		QueueLoop loop;
		TellingBlocks blocks;
		NoCards cards;
		NoReceivers receivers;
		NoHandoffs handoffs;
		NoSinks sinks;
		NoSources sources;
		ResourceRegistry resources(cards, receivers, handoffs, sinks, sources);
		NodeHost host(AudioFormat{ear::SampleRate::fixed<48000>(), 256}, blocks, resources, loop, loop);
		host.nodeAdded(NodeSpec{"amp", "silent", {}}, std::make_shared<SilentConfig>());
		loop.runPosted(); // the flush makes the block
		if (blocks.made == nullptr) ::_exit(92);
		// The block streams: it tells ready, NodeHost drives and carries the plan, and the block's throw interrupts
		// NodeHost's own update.
		Guarded<&TellingBlock::onStreaming>::call(blocks.made, 1);
	});
	INFO(end.written);
	REQUIRE(WIFSIGNALED(end.status));
	CHECK(WTERMSIG(end.status) == SIGABRT);
	const auto record = end.written.find("record TellingBlock::onStreaming fatal=1 node=\n");
	const auto line =
		end.written.find("ear: internal error in TellingBlock::onStreaming: a plan the block cannot carry\n");
	REQUIRE(record != std::string::npos);
	CHECK(record < line);
	const auto terminate = end.written.find("terminate called after throwing an instance of 'std::logic_error'");
	REQUIRE(line != std::string::npos);
	REQUIRE(terminate != std::string::npos);
	CHECK(line < terminate);
	CHECK(end.written.find("the block was told") == std::string::npos);
}

// docs/architecture.md#faults-by-owner: the Faults ear runs with records every fault first, then returns for one an
// adapter owns; one with no owner it ends (the forked child above).
TEST_CASE("The Faults ear runs with records an owned fault, not fatal, and returns", "[boundary]") {
	std::vector<std::string> recorded;
	std::vector<std::string> lines;
	RecordThenEnd faults(
		[&](const InternalError& error) {
			recorded.push_back(std::string(error.callback) + "|" + std::string(error.node) + "|" +
		                       std::string(error.where) + "|" + std::string(error.what) + "|" +
		                       (error.fatal ? "fatal" : "runs on"));
		},
		[&](std::string_view line) { lines.emplace_back(line); });
	faults.caught("FilterPortBlock::onStateChanged", "", "bad state", "hall", true);
	CHECK(recorded == std::vector<std::string>{"FilterPortBlock::onStateChanged|hall||bad state|runs on"});
	CHECK(lines.empty());
}

TEST_CASE("An owner whose callbacks keep throwing is reported and told once", "[boundary]") {
	RecordingFaults faults;
	const Installed installed(faults);
	MainLoopActivity::clear();
	OwnedThrower owner(faults);
	Guarded<&OwnedThrower::onEvent>::call(&owner, 0);
	Guarded<&OwnedThrower::onEvent>::call(&owner, 0);
	Guarded<&OwnedThrower::onEvent>::call(&owner, 0);
	REQUIRE(faults.reports.size() == 2); // the report, and the owner told
	CHECK(faults.reports[0].owned);
	CHECK(faults.reports[1].toOwner);
}

TEST_CASE("The injection seam throws in the one callback it names, once", "[boundary]") {
	RecordingFaults faults;
	const Installed installed(faults);
	MainLoopActivity::clear();
	Thrower thrower;
	boundary::throwInCallback("Thrower::onEvent");
	Guarded<&Thrower::onEvent>::call(&thrower, 1);
	Guarded<&Thrower::onEvent>::call(&thrower, 1);
	REQUIRE(faults.reports.size() == 1);
	CHECK(faults.reports[0].callback == "Thrower::onEvent");
	CHECK(faults.reports[0].detail == "injected");
	CHECK(thrower.ran == 1); // the injected call ran nothing of its member
}
