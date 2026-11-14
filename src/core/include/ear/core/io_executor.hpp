// The framework's one thread for blocking work (docs/architecture.md#threads): disk opens and writes run there, so a
// slow disk delays no node's thread and never the main loop. Types never see it. ear_runtime's IoThread implements it;
// main.cpp makes it at boot and stops it last (docs/architecture.md#stop-sigint-or-sigterm).
#pragma once
#include <functional>
#include <string>
#include "ear/core/executor.hpp"
namespace ear {
/// Runs blocking work on its own thread, one job at a time, in post order (Executor's contract). A job holds, by
/// shared_ptr, what it fills, never a reference into a configuration, and reports through a configuration's channel
/// (NodeHost's), so a job whose configuration ended reports nothing and what it opened is closed by its own destructor.
class IoExecutor : public Executor {
public:
	using Executor::post;
	/// As post(work), with `job` saying what the work is, for the record a stop that finds it still running writes
	/// ("the I/O thread was still busy at exit: <job>").
	virtual void post(std::string job, std::move_only_function<void()> work) = 0;

protected:
	~IoExecutor() = default;
};
} // namespace ear
