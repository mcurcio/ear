#include "ear/output/output_writer.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <chrono>
#include <utility>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include "masked_thread.hpp"

namespace ear::output {

namespace {

/// Writes all of `text`, or gives up on an error other than an interruption (a closed reader): what cannot be written
/// has nowhere else to go.
void writeAll(int descriptor, const std::string& text) {
	std::size_t done = 0;
	while (done < text.size()) {
		const ssize_t wrote = ::write(descriptor, text.data() + done, text.size() - done);
		if (wrote < 0 && errno == EINTR) continue;
		if (wrote <= 0) return;
		done += static_cast<std::size_t>(wrote);
	}
}

/// writeAll for the crash path: never waits more than 200 ms for the reader to take more. Chunks of at most PIPE_BUF:
/// once a pipe polls writable, that much is written without blocking.
void writeBounded(int descriptor, const std::string& text) {
	std::size_t done = 0;
	while (done < text.size()) {
		pollfd ready{descriptor, POLLOUT, 0};
		const int polled = ::poll(&ready, 1, 200);
		if (polled < 0 && errno == EINTR) continue;
		if (polled <= 0 || (ready.revents & POLLOUT) == 0) return;
		const ssize_t wrote =
			::write(descriptor, text.data() + done, std::min<std::size_t>(PIPE_BUF, text.size() - done));
		if (wrote < 0 && errno == EINTR) continue;
		if (wrote <= 0) return;
		done += static_cast<std::size_t>(wrote);
	}
}

} // namespace

OutputWriter::OutputWriter(int descriptor, std::size_t capacity, std::size_t byteCapacity)
	: descriptor_(descriptor), capacity_(capacity), byteCapacity_(byteCapacity),
	  thread_(maskedThread([this] { run(); })) {}

OutputWriter::~OutputWriter() { stop(); }

bool OutputWriter::offer(std::string line) {
	{
		const std::scoped_lock lock(mutex_);
		if (stopping_ || queue_.size() >= capacity_) return false;
		// A line past the byte bound is refused only when others wait, so the largest record still gets through alone.
		if (!queue_.empty() && queuedBytes_ + line.size() + 1 > byteCapacity_) return false;
		line += '\n';
		queuedBytes_ += line.size();
		queue_.push_back(std::move(line));
	}
	wake_.notify_one();
	return true;
}

void OutputWriter::spill() noexcept {
	// Bounded waits: the dying thread may be the writer itself, or hold the queue's lock. The order is run()'s: the
	// queue, then the writing.
	const std::unique_lock lock(mutex_, std::chrono::milliseconds(200));
	if (!lock) return;
	const std::unique_lock writing(writeMutex_, std::chrono::milliseconds(200));
	if (!writing) return;
	std::string batch;
	for (const std::string& line : queue_) batch += line;
	queue_.clear();
	queuedBytes_ = 0;
	writeBounded(descriptor_, batch);
}

void OutputWriter::stop() {
	{
		const std::scoped_lock lock(mutex_);
		stopping_ = true;
	}
	wake_.notify_one();
	if (thread_.joinable()) thread_.join();
}

void OutputWriter::run() {
	std::unique_lock lock(mutex_);
	while (true) {
		wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
		if (queue_.empty()) return; // stopping, and everything is written
		// Take every waiting line and write them in one go, outside the lock, so offer never waits on the write, nor on
		// the batch being joined.
		std::deque<std::string> taken;
		std::swap(taken, queue_);
		queuedBytes_ = 0;
		std::unique_lock writing(writeMutex_); // before the lock is let go: spill() never writes in between
		lock.unlock();
		std::string batch;
		for (const std::string& line : taken) batch += line;
		writeAll(descriptor_, batch);
		writing.unlock();
		lock.lock();
	}
}

} // namespace ear::output
