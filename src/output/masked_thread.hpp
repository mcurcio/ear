// Private to ear_output: a thread started with every signal blocked. ear's own threads start before Engine::create
// blocks the stop signals (they exist before any engine thread, so the descriptors are captured first), and a stop
// signal delivered to one of them would end the process by its default action; a write to a closed reader gives EPIPE
// instead of SIGPIPE. The synchronous fault signals (SIGSEGV, SIGBUS, SIGFPE, SIGILL) stay unblocked: one blocked when
// its thread faults is delivered with its default action, skipping the crash handler's last words
// (DescriptorCapture::onCrash), which runs on whichever thread faulted.
#pragma once
#include <csignal>
#include <thread>
#include <utility>
#include <pthread.h>
namespace ear::output {
template <typename Body> std::thread maskedThread(Body&& body) {
	sigset_t all;
	sigset_t previous;
	sigfillset(&all);
	for (const int fault : {SIGSEGV, SIGBUS, SIGFPE, SIGILL}) sigdelset(&all, fault);
	pthread_sigmask(SIG_BLOCK, &all, &previous);
	std::thread thread(std::forward<Body>(body));
	pthread_sigmask(SIG_SETMASK, &previous, nullptr);
	return thread;
}
} // namespace ear::output
