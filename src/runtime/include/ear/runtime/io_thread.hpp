// The I/O thread (docs/architecture.md#threads): core's IoExecutor on one ordinary thread, "ear-io", made by main.cpp
// at boot and stopped last, after every node and the keeper (docs/architecture.md#stop-sigint-or-sigterm).
#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "ear/core/io_executor.hpp"
namespace ear::runtime {
/// How long a stop waits for the job in flight before it reports it still busy.
inline constexpr std::chrono::milliseconds ioStopGrace{2'000};

class IoThread final : public IoExecutor {
public:
	/// Starts the thread; std::system_error when it cannot.
	IoThread();
	/// stop(), waiting as long as the job in flight takes, then the join. A stop that reported a job still busy leaves
	/// a thread no destructor should wait on: the process ends instead (std::_Exit, as main.cpp does).
	~IoThread();
	IoThread(const IoThread&) = delete;
	IoThread& operator=(const IoThread&) = delete;

	/// Any thread; never waits for the job. Jobs run one at a time in post order; a job that throws ends the process
	/// (std::terminate: a job is the framework's code, and what it fills is never left half-told). Posted after a stop:
	/// destroyed unrun, on the calling thread.
	void post(std::string job, std::move_only_function<void()> work) override;
	void post(std::move_only_function<void()> work) override;
	/// The thread's own. Main thread, once nothing posts any more (every node gone): queued jobs are destroyed unrun
	/// here, and the job in flight gets `grace` to finish. Nothing once the thread has ended and been joined; else the
	/// job still running, by the name it was posted with. Called again, it waits again.
	std::optional<std::string> stop(std::chrono::milliseconds grace = ioStopGrace);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace ear::runtime
