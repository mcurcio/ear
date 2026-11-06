// A sendspin node's own threads (docs/design/sendspin.md section 6): the worker, the library's "main loop", and the
// notifier, telling the library what played. They own the library's client; nothing else touches it, so it never
// meets a realtime thread or PipeWire's main loop.
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include "ear/core/node_type.hpp"
#include "feed.hpp"
#include "mailbox.hpp"
#include "sendspin_code.hpp"
#include "settings.hpp"
namespace ear::sendspin {
class SendspinWorker {
public:
  /// Starts the threads; `feed` and `events` outlive `stopped`. Throws std::system_error when it cannot.
  SendspinWorker(SendspinSettings settings, AudioFormat format, Feed& feed, InsidesEvents& events, std::uint32_t feedFrames);
  void stop(std::move_only_function<void()> stopped);   // main thread; the insides' close()
  ~SendspinWorker();                                    // after `stopped`; joins a finished thread
  int bell() const noexcept;                            // the eventfd the code rings
  Mailbox<PlayProgress>& progress() noexcept;
  std::int64_t clientTimeOf(std::int64_t serverUs) const;   // the tests' window on the time filter
private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
