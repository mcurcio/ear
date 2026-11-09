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
#include <memory>
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
};

/// Records instead of ending the process (R4a's main.cpp ends it), so a test sees what the boundary told it and what
/// ran after.
class RecordingFaults final : public Faults {
public:
	void caught(std::string_view callback, std::string_view where, std::string_view detail,
	            bool owned) noexcept override {
		reports.push_back(Report{std::string(callback), std::string(where), std::string(detail), owned, false});
	}
	std::vector<Report> reports;
};

/// Installs `faults` for one test made without an engine, and takes it away after.
class Installed {
public:
	explicit Installed(Faults& faults) { boundary::install(&faults); }
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

class OwnedThrower final : public FaultOwner {
public:
	explicit OwnedThrower(RecordingFaults& faults) : faults_(faults) {}
	void faulted(std::string_view callback, std::string_view detail) noexcept override {
		faults_.reports.push_back(Report{std::string(callback), {}, std::string(detail), true, true});
	}
	void onEvent(std::uint64_t /*count*/) { throw std::runtime_error("owned boom"); }

private:
	RecordingFaults& faults_;
};

std::unique_ptr<Engine> makeEngine(Faults* faults) {
	for (const char* name : {"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE", "DBUS_SESSION_BUS_ADDRESS",
	                         "DBUS_SYSTEM_BUS_ADDRESS"}) {
		unsetenv(name);
	}
	auto engine = Engine::create(EngineSettings{EAR_ENGINE_CONF, 48000, 256, faults});
	if (!engine) FAIL("Engine::create: " << engine.error().detail);
	return std::move(*engine);
}

/// Runs the main loop until something calls engine.quit(); the deadline only fails the test. Its timer is registered
/// through the boundary too.
class Deadline {
public:
	explicit Deadline(Engine& engine) : engine_(engine) {}
	bool run(std::chrono::seconds deadline = std::chrono::seconds(5)) {
		spa_source* timer = addTimer(engine_.mainLoop(), guarded<&Deadline::onTimeout>, this);
		timespec value{static_cast<time_t>(deadline.count()), 0};
		pw_loop_update_timer(engine_.mainLoop(), timer, &value, nullptr, false);
		engine_.run();
		pw_loop_destroy_source(engine_.mainLoop(), timer);
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

TEST_CASE("A FaultOwner's callback that throws is reported, then the owner is told", "[boundary]") {
	RecordingFaults faults;
	const Installed installed(faults);
	OwnedThrower owner(faults);
	Guarded<&OwnedThrower::onEvent>::call(&owner, 0);
	REQUIRE(faults.reports.size() == 2);
	CHECK(faults.reports[0].callback == "OwnedThrower::onEvent");
	CHECK(faults.reports[0].owned);
	CHECK_FALSE(faults.reports[0].toOwner);
	CHECK(faults.reports[1].toOwner);
	CHECK(faults.reports[1].callback == "OwnedThrower::onEvent");
	CHECK(faults.reports[1].detail == "owned boom");
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
