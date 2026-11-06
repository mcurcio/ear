// Slice S4: the realtime to ordinary-thread handoff (docs/design/m1-white-to-file.md
// sections 1 and 2). JACK's disk-writer pattern: the realtime writer interleaves into a
// FrameRing and wakes the handoff's own pw_thread_loop with pw_loop_signal_event; that
// thread hands what is in the ring to the consumer. A full ring drops frames and counts them.
#include "ear/pipewire/handoffs.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <string>
#include <system_error>
#include <utility>

#include <pipewire/pipewire.h>

#include "ear/core/frame_ring.hpp"

namespace ear::pipewire {
namespace {

class ThreadHandoff final : public Handoff, public RtWriter {
public:
  ThreadHandoff(pw_loop* mainLoop, const NodeName& node, std::uint32_t channels,
                std::uint32_t capacityFrames, HandoffConsumer& consumer)
      : mainLoop_(mainLoop),
        ring_(channels, capacityFrames),
        consumer_(consumer) {
    const std::string name = "ear-handoff " + node;
    thread_ = pw_thread_loop_new(name.c_str(), nullptr);
    if (thread_ == nullptr) throw std::system_error(errno, std::generic_category(), name);
    wake_ = pw_loop_add_event(pw_thread_loop_get_loop(thread_), &ThreadHandoff::onWake, this);
    finished_ = pw_loop_add_event(mainLoop_, &ThreadHandoff::onFinished, this);
    if (const int result = pw_thread_loop_start(thread_); result < 0) {
      destroy();
      throw std::system_error(-result, std::generic_category(), name);
    }
  }

  ~ThreadHandoff() override { destroy(); }

  ThreadHandoff(const ThreadHandoff&) = delete;
  ThreadHandoff& operator=(const ThreadHandoff&) = delete;

  RtWriter& writer() noexcept override { return *this; }

  void close(std::move_only_function<void(std::uint64_t droppedFrames)> closed) override {
    closed_ = std::move(closed);
    // The writing port block is gone, so every write happened before this store.
    closing_.store(true, std::memory_order_release);
    pw_loop_signal_event(pw_thread_loop_get_loop(thread_), wake_);
  }

  std::uint32_t write(std::span<const std::span<const float>> channels,
                      std::uint32_t frames) noexcept override {
    const std::uint32_t accepted = ring_.writePlanar(channels, frames);
    if (accepted < frames) dropped_.fetch_add(frames - accepted, std::memory_order_relaxed);
    if (accepted > 0) pw_loop_signal_event(pw_thread_loop_get_loop(thread_), wake_);
    return accepted;
  }

private:
  /// The handoff's thread: everything in the ring to the consumer, in write order.
  void drain() {
    for (;;) {
      // Up to the ring's end; the rest (wrapped to its start) on the next pass.
      const std::span<const float> run = ring_.readSpan();
      if (run.empty()) return;
      const auto frames = static_cast<std::uint32_t>(run.size() / ring_.channels());
      consumer_.consume(run, frames);
      ring_.skip(frames);
    }
  }

  static void onWake(void* data, std::uint64_t /*count*/) {
    auto* self = static_cast<ThreadHandoff*>(data);
    // Read before draining: once closing, the drain below sees every write.
    const bool closing = self->closing_.load(std::memory_order_acquire);
    self->drain();
    if (closing && !self->consumerFinished_) {
      self->consumerFinished_ = true;
      self->consumer_.finish();
      pw_loop_signal_event(self->mainLoop_, self->finished_);
    }
  }

  /// The main loop, after the consumer's finish(). The handoff's thread, idle now, is
  /// stopped by the destructor, never here: `closed` must not wait on the consumer. `closed`
  /// may destroy this handoff, so it is the last thing done here.
  static void onFinished(void* data, std::uint64_t /*count*/) {
    auto* self = static_cast<ThreadHandoff*>(data);
    if (!self->closed_) return;
    auto closed = std::exchange(self->closed_, {});
    closed(self->dropped_.load(std::memory_order_relaxed));
  }

  void destroy() noexcept {
    if (thread_ == nullptr) return;
    pw_thread_loop_stop(thread_);
    if (wake_ != nullptr) pw_loop_destroy_source(pw_thread_loop_get_loop(thread_), wake_);
    pw_thread_loop_destroy(thread_);
    thread_ = nullptr;
    if (finished_ != nullptr) pw_loop_destroy_source(mainLoop_, finished_);
  }

  pw_loop* mainLoop_;
  FrameRing ring_;
  HandoffConsumer& consumer_;
  pw_thread_loop* thread_ = nullptr;
  spa_source* wake_ = nullptr;       // on the handoff's thread loop
  spa_source* finished_ = nullptr;   // on the main loop
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<bool> closing_{false};
  bool consumerFinished_ = false;    // handoff thread only
  std::move_only_function<void(std::uint64_t)> closed_;   // main thread only
};

}  // namespace

ThreadHandoffs::ThreadHandoffs(pw_loop* mainLoop) : mainLoop_(mainLoop) {}

std::unique_ptr<Handoff> ThreadHandoffs::make(const NodeName& node, std::uint32_t channels,
                                              std::uint32_t capacityFrames,
                                              HandoffConsumer& consumer) {
  return std::make_unique<ThreadHandoff>(mainLoop_, node, channels, capacityFrames, consumer);
}

}  // namespace ear::pipewire
