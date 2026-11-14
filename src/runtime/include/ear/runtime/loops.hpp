// The nodes' loops on real threads (docs/architecture.md#threads; docs/writing-a-node-type.md#loops): core's LoopAdapter,
// strands on one pool and each dedicated loop on a thread of its own, "ear-<name> <node>". main.cpp makes it at boot;
// NodeHost makes, finishes and closes each loop. Timers run on CLOCK_MONOTONIC, watches on epoll.
#pragma once
#include <chrono>
#include <cstddef>
#include <memory>
#include <string_view>
#include "ear/core/executor.hpp"
#include "ear/core/loop.hpp"
namespace ear::runtime {

/// The strand pool's sizes (docs/writing-a-node-type.md#loops).
struct LoopOptions {
	/// The pool's threads: 0 is min(4, the hardware's threads). Started with the first strand.
	std::size_t strandThreads = 0;
	/// A strand handler past this is told slow (LoopEvents::slow), once per loop.
	std::chrono::milliseconds slowBound = loopSlowBound;
	/// A strand handler past this gets a replacement thread, so the other strands keep running.
	std::chrono::milliseconds replaceAfter{1'000};
	/// The most threads the pool runs, replacements included.
	std::size_t maxThreads = 16;
};

class Loops final : public LoopAdapter {
public:
	/// `mainLoop` (outliving every loop) is where each loop's events, finished and closed are posted.
	explicit Loops(Executor& mainLoop, LoopOptions options = {});
	/// The pool's threads stopped and joined. Precondition: every loop made was closed and destroyed.
	~Loops();
	Loops(const Loops&) = delete;
	Loops& operator=(const Loops&) = delete;

	/// Main thread. A strand joins the pool (started with the first); a dedicated loop starts its thread. Ready, or
	/// failed with the errno's text when no thread could start, is posted to the main loop.
	std::unique_ptr<NodeLoop> make(const NodeName& node, std::string_view name, LoopKind kind,
	                               LoopEvents& events) override;
	/// Any thread: the threads the pool runs now (its workers, replacements included; not its reactor).
	std::size_t poolThreads() const;

private:
	Executor& mainLoop_;
	LoopOptions options_;
	std::shared_ptr<void> pool_; // the strand pool (loops.cpp), made with the first strand
};

} // namespace ear::runtime
