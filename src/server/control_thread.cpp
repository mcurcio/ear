// The control thread: one io_context, one thread (docs/design/batch2.md sections 2 and 4.6,
// control_thread.hpp). Posted work waits in this class's own queue, one Asio handler per item,
// so that stop() can destroy whatever is left unrun at once (an io_context destroys its queued
// handlers only when it is itself destroyed).
#include "ear/server/control_thread.hpp"

#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>

#include "control_context.hpp"

namespace ear::server {
namespace {

thread_local boost::asio::io_context* runningContext = nullptr;

} // namespace

boost::asio::io_context& currentControlContext() {
	if (runningContext == nullptr) throw std::logic_error("currentControlContext: not on a control thread");
	return *runningContext;
}

struct ControlThread::Impl {
	boost::asio::io_context io{1};
	boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work = boost::asio::make_work_guard(io);
	std::mutex mutex;
	bool stopped = false;                                 // guarded by mutex
	std::deque<std::move_only_function<void()>> queue;   // guarded by mutex
	std::thread thread;

	/// On the control thread: the oldest posted work (one handler is posted per item).
	void runOne() {
		std::move_only_function<void()> next;
		{
			const std::scoped_lock lock(mutex);
			if (queue.empty()) return;   // stop() has destroyed it
			next = std::move(queue.front());
			queue.pop_front();
		}
		next();
	}
};

ControlThread::ControlThread() : impl_(std::make_unique<Impl>()) {
	impl_->thread = std::thread([impl = impl_.get()] {
		runningContext = &impl->io;
		impl->io.run();
		runningContext = nullptr;
	});
}

ControlThread::~ControlThread() { stop(); }

void ControlThread::post(std::move_only_function<void()> work) {
	const std::scoped_lock lock(impl_->mutex);   // one lock over both queues keeps post order
	if (impl_->stopped) return;                  // `work` is destroyed unrun as it leaves scope
	impl_->queue.push_back(std::move(work));
	boost::asio::post(impl_->io, [impl = impl_.get()] { impl->runOne(); });
}

void ControlThread::stop() {
	{
		const std::scoped_lock lock(impl_->mutex);
		if (impl_->stopped) return;
		impl_->stopped = true;
	}
	impl_->work.reset();
	impl_->io.stop();
	if (impl_->thread.joinable()) impl_->thread.join();
	std::deque<std::move_only_function<void()>> left;
	{
		const std::scoped_lock lock(impl_->mutex);
		left.swap(impl_->queue);
	}
	// `left` is destroyed here, unrun, on the stopping thread.
}

} // namespace ear::server
