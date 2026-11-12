#pragma once
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
struct pw_loop;
struct pw_context;
struct pw_core;
namespace ear {
class CutRecord;
}
namespace ear::pipewire {
class Faults;
struct EngineSettings {
	std::filesystem::path configFile; // engine.conf; must not set default.clock.*
	std::uint32_t graphFrequency;     // -> default.clock.rate and the only allowed rate
	std::uint32_t blockSize;          // -> default.clock.quantum = min-quantum = max-quantum
	/// Where an exception caught at the callback boundary is reported while the engine lives (faults.hpp); it must
	/// outlive the engine. None: a line naming the callback on standard error, then std::terminate.
	Faults* faults = nullptr;
};
struct EngineError {
	std::string detail;
};
/// How the thread that runs the port blocks' process() is scheduled.
struct Scheduling {
	bool realtime;
	std::string detail;
}; // detail: "SCHED_FIFO 88" etc.
/// PipeWire's engine inside EAR: main loop, context, EAR's own internal connection. No socket
/// in M1 (core.daemon=false). Main thread only.
class Engine {
public:
	/// If CAP_SYS_NICE is in the process's permitted set, it is made effective before the
	/// context loads its modules (so module-rt can raise the data thread); absent: no error.
	/// The data thread flushes denormals to zero (FTZ and DAZ) from before the first process().
	static std::expected<std::unique_ptr<Engine>, EngineError> create(const EngineSettings& settings);
	~Engine();
	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;
	void run();                                       // runs the main loop until quit()
	void quit();                                      // from a main-loop callback
	void onStopSignal(std::function<void()> handler); // SIGINT/SIGTERM, as main-loop events
	/// Main thread. How the thread that runs the port blocks' process() is scheduled.
	Scheduling dataThreadScheduling() const;
	/// Which graph cycles the internal clock cut short, counted from the engine's start (docs/engine.md#graphcycle):
	/// written on the data thread, read on any by CutRecord's contract. For this package's adapters and its tests.
	const CutRecord& cuts() const noexcept;
	/// The internal clock's global id: a cycle whose clock.id is this one is numbered by cuts().
	std::uint32_t internalClockId() const noexcept;
	pw_loop* mainLoop() const noexcept; // for this package's adapters only
	pw_context* context() const noexcept;
	pw_core* core() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	explicit Engine(std::unique_ptr<Impl> impl);
};
} // namespace ear::pipewire
