// The output thread (docs/design/node-framework.md section 8.8): the one writer of ear's real standard output. The main
// loop never blocks on output: lines reach this thread through a bounded queue, and a full queue refuses a line
// instead of waiting (the encoder counts what it refused).
#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
namespace ear::output {
class OutputWriter {
public:
	/// Writes to `descriptor`, which it neither owns nor closes, on its own thread (every signal blocked, so a stop
	/// signal never lands on it and a closed reader gives EPIPE, not SIGPIPE). At most `capacity` lines wait, and at
	/// most `byteCapacity` bytes of them (unless one line alone is larger).
	explicit OutputWriter(int descriptor, std::size_t capacity = 4096, std::size_t byteCapacity = 4 << 20);
	~OutputWriter(); // stop()
	OutputWriter(const OutputWriter&) = delete;
	OutputWriter& operator=(const OutputWriter&) = delete;
	/// Any thread; never waits on the write. Queues `line` and its newline; false when the queue is full or the
	/// writer has stopped.
	bool offer(std::string line);
	/// Writes every queued line, then ends the thread. Idempotent.
	void stop();
	/// The crash path (DescriptorCapture::onCrash): writes what is still queued now, on the calling thread, unless the
	/// writer or the queue stays busy for 200 ms (the dying thread may hold either); gives up when the reader takes
	/// nothing for 200 ms. What it took from the queue is gone either way.
	void spill() noexcept;

private:
	void run();

	int descriptor_;
	std::size_t capacity_;
	std::size_t byteCapacity_;
	std::size_t queuedBytes_ = 0; // under mutex_
	std::timed_mutex mutex_;
	std::timed_mutex writeMutex_; // held while a batch is written
	std::condition_variable_any wake_;
	std::deque<std::string> queue_;
	bool stopping_ = false;
	std::thread thread_;
};
} // namespace ear::output
