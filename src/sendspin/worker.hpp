// A sendspin node's own threads (docs/design/sendspin.md section 6): the worker, the library's "main loop", and the
// notifier, telling the library what played. They own the library's client and the node's key file; nothing else
// touches the client but the handoff's thread, through the consumer here, so it never meets a realtime thread or
// PipeWire's main loop.
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include "ear/core/node_type.hpp"
#include "ear/folder/confined_folder.hpp"
#include "feed.hpp"
#include "mailbox.hpp"
#include "sendspin_code.hpp"
#include "settings.hpp"
namespace ear::sendspin {
class SendspinWorker {
public:
  /// Starts the threads; `feed` and `events` outlive `stopped`. Throws std::system_error when it cannot. The node's
  /// key file lives in `folder`.
  SendspinWorker(std::shared_ptr<const folder::ConfinedFolder> folder, NodeName node, SendspinSettings settings,
                 AudioFormat format, Feed& feed, InsidesEvents& events, std::uint32_t feedFrames);
  void stop(std::move_only_function<void()> stopped);   // main thread; the insides' close()
  ~SendspinWorker();                                    // after `stopped`; joins a finished thread
  int bell() const noexcept;                            // the eventfd the code rings
  Mailbox<PlayProgress>& progress() noexcept;
  HandoffConsumer& toServer() noexcept;                 // the handoff's far end: write_audio; closed before stop()
  Mailbox<CaptureAnchor>& capture() noexcept;           // when what the handoff holds was captured
  std::int64_t clientTimeOf(std::int64_t serverUs) const;   // the tests' window on the time filter
private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
