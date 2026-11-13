// The graph file keeper's one background thread (docs/architecture.md#how-a-change-moves): an executor that also
// watches the graph file folder. It waits in poll() on an eventfd (posted work) and an inotify descriptor (the folder),
// with no timeout: events, never timers.
#pragma once
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "ear/core/executor.hpp"
namespace ear::config {
class KeeperThread final : public Executor {
public:
	struct Handlers {
		std::move_only_function<void()> diskEvent; // inotify named `fileName`, overflowed, or the folder moved or went
		std::move_only_function<void()> free;      // after each wake's work, once nothing posted is left
	};
	/// Starts the thread. It arms an inotify watch on `directory` (IN_CLOSE_WRITE | IN_MOVED_TO |
	/// IN_MOVED_FROM | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF) before it runs any posted
	/// work, then waits in poll() on an eventfd and the inotify fd, with no timeout. Error: the watch
	/// could not be armed. Start it after Engine::create, so it inherits the blocked stop signals.
	static std::expected<std::unique_ptr<KeeperThread>, std::string> start(std::string directory, std::string fileName,
	                                                                       Handlers handlers);
	~KeeperThread(); // stop()
	KeeperThread(const KeeperThread&) = delete;
	KeeperThread& operator=(const KeeperThread&) = delete;
	void post(std::move_only_function<void()> work) override; // any thread; post order
	/// Any thread; idempotent. Ends the folder watch once the work already posted has run: no disk event follows, and
	/// posted work still runs. The keeper calls it as it stops (docs/architecture.md#stop-sigint-or-sigterm): closing
	/// an inotify descriptor that still holds a watch waits for the kernel to reap it, so the watch is removed here,
	/// while the nodes stop, and the close at the end finds nothing left to wait for.
	void unwatch();
	/// Any thread but its own; idempotent. Runs the work already posted, ends and joins. Later posts are destroyed unrun.
	void stop();

private:
	KeeperThread(int wake, int watch, int watched, std::string fileName, Handlers handlers);
	void run();
	bool namesFile(const char* buffer,
	               long size) const; // the inotify events read name the file, or the folder itself
	int wake_;                       // eventfd
	int watch_;                      // inotify
	int watched_;                    // the folder's watch, or -1 once unwatch() ran (its own thread only)
	std::string fileName_;
	Handlers handlers_;
	std::mutex mutex_;
	std::deque<std::move_only_function<void()>> queue_; // guarded by mutex_
	bool stopping_ = false;                             // guarded by mutex_
	std::thread thread_;
};
} // namespace ear::config
