#include "ear/output/output_writer.hpp"

#include <cerrno>
#include <utility>
#include <vector>

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

} // namespace

OutputWriter::OutputWriter(int descriptor, std::size_t capacity)
	: descriptor_(descriptor), capacity_(capacity), thread_(maskedThread([this] { run(); })) {}

OutputWriter::~OutputWriter() { stop(); }

bool OutputWriter::offer(std::string line) {
	{
		const std::scoped_lock lock(mutex_);
		if (stopping_ || queue_.size() >= capacity_) return false;
		line += '\n';
		queue_.push_back(std::move(line));
	}
	wake_.notify_one();
	return true;
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
		// Take every waiting line and write them in one go, outside the lock, so offer never waits on the write.
		std::string batch;
		while (!queue_.empty()) {
			batch += queue_.front();
			queue_.pop_front();
		}
		lock.unlock();
		writeAll(descriptor_, batch);
		lock.lock();
	}
}

} // namespace ear::output
