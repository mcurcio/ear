// ear's own pw_log and alsa-lib's error handler (docs/output-records.md): PipeWire's logging, every
// SPA plugin's through it, and alsa-lib's errors, as log entries. PipeWire logs from its data loop (xruns among them)
// and alsa-lib fails on whatever thread called it, so a write here never formats into the heap, takes a lock or blocks:
// it fills a fixed slot of a lock-free ring and rings a bell (one eventfd write, at most once until the next drain). A
// full ring drops the entry and counts it. The drain, on a thread of the caller's (the descriptor capture's), turns
// slots into entries.
#pragma once
#include "ear/core/log.hpp"
namespace ear::pipewire {
class EngineLog {
public:
	/// Makes ear's log PipeWire's process-wide log and alsa-lib's error handler (reached by name in libasound.so.2,
	/// which the image carries without its headers), both through the callback boundary: before Engine::create, whose pw_init keeps a log
	/// already set; again before each engine, since PipeWire's teardown may put its own back. Main thread. The log
	/// lives as long as the process.
	static EngineLog& install();
	/// Readable while entries wait.
	int bell() const noexcept;
	/// One thread at a time: every waiting entry to `sink` as an unattributed entry, from "pipewire" with its topic or
	/// from "alsa-lib"; then, if the ring dropped any since the last drain, one entry from "ear" that counts them.
	void drain(LogSink& sink);

	EngineLog(const EngineLog&) = delete;
	EngineLog& operator=(const EngineLog&) = delete;

private:
	EngineLog();
};
} // namespace ear::pipewire
