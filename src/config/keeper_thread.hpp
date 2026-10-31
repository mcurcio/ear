// The graph file keeper's one background thread (docs/design/knobly-config.md, "Module map and
// threads"; S4): an executor that also watches the graph file folder. It waits in poll() on an
// eventfd (posted work) and an inotify descriptor (the folder), with no timeout: events, never timers.
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
    std::move_only_function<void()> diskEvent;   // inotify named `fileName`, overflowed, or the folder moved or went
    std::move_only_function<void()> free;        // after each wake's work, once nothing posted is left
  };
  /// Starts the thread. It arms an inotify watch on `directory` (IN_CLOSE_WRITE | IN_MOVED_TO |
  /// IN_MOVED_FROM | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF) before it runs any posted
  /// work, then waits in poll() on an eventfd and the inotify fd, with no timeout. Error: the watch
  /// could not be armed. Start it after Engine::create, so it inherits the blocked stop signals.
  static std::expected<std::unique_ptr<KeeperThread>, std::string> start(std::string directory, std::string fileName, Handlers handlers);
  ~KeeperThread();   // stop()
  KeeperThread(const KeeperThread&) = delete;
  KeeperThread& operator=(const KeeperThread&) = delete;
  void post(std::move_only_function<void()> work) override;   // any thread; post order
  /// Any thread but its own; idempotent. Runs the work already posted, ends and joins. Later posts are destroyed unrun.
  void stop();
private:
  KeeperThread(int wake, int watch, std::string fileName, Handlers handlers);
  void run();
  bool namesFile(const char* buffer, long size) const;   // the inotify events read name the file, or the folder itself
  int wake_;    // eventfd
  int watch_;   // inotify
  std::string fileName_;
  Handlers handlers_;
  std::mutex mutex_;
  std::deque<std::move_only_function<void()>> queue_;   // guarded by mutex_
  bool stopping_ = false;                               // guarded by mutex_
  std::thread thread_;
};
}
