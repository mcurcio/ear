// Every write to the process's descriptors 1 and 2 captured (docs/design/node-framework.md section 8.8): a grep of
// src/nodes/ cannot see what PipeWire, alsa-lib or a library writes inside ear's process, so at boot, before any
// thread starts, ear keeps a duplicate of the real standard output for its encoder's writer alone and puts a pipe on
// each of descriptors 1 and 2. One thread reads both pipes, a line at a time, and hands each line on (main makes it an
// unattributed log record). The same thread waits on other descriptors it is given (a log ring's bell) and calls
// their handlers, so a ring filled where nothing may block is drained here.
//
// A process that dies abnormally would lose its last words in the pipes (abort's, std::terminate's "terminate called
// after throwing ...", assert's, a sanitizer's): while a capture is installed, a handler for std::terminate and for
// SIGABRT, SIGSEGV, SIGBUS, SIGFPE and SIGILL writes what the pipes still hold, as raw bytes, to the real descriptors,
// puts them back on 1 and 2, and lets the crash go on as it would have (the handler and dispositions found at install).
// std::terminate also runs onCrash's hook first (the output thread's queue). A child process must get its own standard
// error, never inherit descriptor 2 (node-framework.md section 7.5): an inherited pipe end would outlive the capture.
#pragma once
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
namespace ear::output {
class DescriptorCapture {
public:
	struct Line {
		std::string_view source; // "stdout" | "stderr": the descriptor it was written to
		std::string text;        // without its newline
		bool cut;                // longer than lineBound: the rest of that line was dropped
	};
	static constexpr std::size_t lineBound = 4096;

	/// Main thread, before any other thread starts: keeps a duplicate of descriptors 1 and 2, then puts a pipe on
	/// each; stdout's C stream becomes line-buffered, so what a library prints reaches the pipe a line at a time.
	/// An error names the call that failed, with nothing changed.
	static std::expected<std::unique_ptr<DescriptorCapture>, std::string> install();
	~DescriptorCapture(); // stop()
	DescriptorCapture(const DescriptorCapture&) = delete;
	DescriptorCapture& operator=(const DescriptorCapture&) = delete;

	/// The real standard output, for the encoder's writer alone; close-on-exec, owned by this object.
	int realOutput() const noexcept { return realOutput_; }
	/// Before start: also waits on `descriptor` (not owned) and calls `onReady` on the capture thread whenever it is
	/// readable, and once more as the capture stops. `onReady` must make it unreadable again (read its eventfd).
	void watch(int descriptor, std::function<void()> onReady);
	/// Before start: what std::terminate runs first on the crash path (write out the records still queued). It must not
	/// wait without bound, since the dying thread may hold any lock.
	void onCrash(std::function<void()> hook);
	/// The crash path's call of the onCrash hook; never throws. Not for other callers.
	void beforeCrash() noexcept;
	/// Starts the capture thread (every signal but the faults blocked); each captured line goes to `onLine` on it, in
	/// order per descriptor.
	void start(std::function<void(Line)> onLine);
	/// Puts the real descriptors back on 1 and 2, so a write after this goes where it went before ear started; reads
	/// what the pipes still hold (a last partial line included) and calls every watched handler once; ends the thread,
	/// even while a duplicate of descriptor 1 or 2 keeps a pipe's write end open; disarms the crash path. Idempotent.
	void stop();

private:
	DescriptorCapture() = default;
	void run();

	int realOutput_ = -1;
	int realError_ = -1;
	int outputPipe_ = -1; // read ends
	int errorPipe_ = -1;
	int wake_ = -1; // stop()'s bell: an eventfd
	bool armed_ = false;
	std::function<void()> onCrash_;
	struct Watched {
		int descriptor;
		std::function<void()> onReady;
	};
	std::vector<Watched> watched_;
	std::function<void(Line)> onLine_;
	std::thread thread_;
	bool stopped_ = false;
};
} // namespace ear::output
