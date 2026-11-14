#include "ear/runtime/io_thread.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include <pthread.h>

namespace ear::runtime {
namespace {
struct Job {
	std::string name;
	std::move_only_function<void()> work;
};
} // namespace

struct IoThread::Impl {
	std::mutex mutex;
	std::condition_variable changed;
	std::deque<Job> queue;              // mutex
	bool stopping = false;              // mutex
	bool ended = false;                 // mutex: the thread's loop has returned
	std::optional<std::string> running; // mutex: the job in flight
	std::thread thread;                 // last: started once the rest exists

	void run() {
		std::unique_lock lock(mutex);
		for (;;) {
			changed.wait(lock, [&] { return stopping || !queue.empty(); });
			if (stopping) break; // what is queued is the stop's to destroy
			Job job = std::move(queue.front());
			queue.pop_front();
			running = job.name;
			lock.unlock();
			job.work();
			job = {}; // what the job held goes before it counts as done (a descriptor it opened closes here)
			lock.lock();
			running.reset();
		}
		ended = true;
		changed.notify_all();
	}
	/// Any thread: no job starts from now on; the jobs not yet run, taken out of the queue for the caller to destroy
	/// outside the lock.
	std::deque<Job> beginStop() {
		std::deque<Job> dropped;
		{
			const std::scoped_lock lock(mutex);
			stopping = true;
			dropped.swap(queue);
		}
		changed.notify_all();
		return dropped;
	}
};

IoThread::IoThread() : impl_(std::make_unique<Impl>()) {
	impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
	// The name ps and a debugger show (at most 15 bytes); a failure only leaves the process's name.
	::pthread_setname_np(impl_->thread.native_handle(), "ear-io");
}

IoThread::~IoThread() {
	impl_->beginStop().clear();
	if (impl_->thread.joinable()) impl_->thread.join();
}

void IoThread::post(std::string job, std::move_only_function<void()> work) {
	{
		const std::scoped_lock lock(impl_->mutex);
		if (!impl_->stopping) {
			impl_->queue.push_back(Job{std::move(job), std::move(work)});
			impl_->changed.notify_all();
			return;
		}
	}
	// After a stop: `work` is destroyed unrun here, outside the lock, when this call returns.
}

void IoThread::post(std::move_only_function<void()> work) { post("a job", std::move(work)); }

std::optional<std::string> IoThread::stop(std::chrono::milliseconds grace) {
	impl_->beginStop().clear(); // destroyed unrun on this thread, outside the lock
	std::unique_lock lock(impl_->mutex);
	if (!impl_->changed.wait_for(lock, grace, [&] { return impl_->ended; }))
		return impl_->running.value_or("the thread's own turn");
	lock.unlock();
	if (impl_->thread.joinable()) impl_->thread.join();
	return std::nullopt;
}

} // namespace ear::runtime
