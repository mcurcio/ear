// Slice S4's tests (docs/design/m1-white-to-file.md section 3, acceptance criteria S4): the
// real ThreadHandoffs on PipeWire's own loops, no engine. Every wait is event-gated: a test
// waits for the event it needs; the deadline only fails the test.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <thread>
#include <vector>

#include <pipewire/pipewire.h>

#include "catch2/catch.hpp"
#include "ear/pipewire/handoffs.hpp"

// Counts allocations made by a thread that asks for it (write() must make none). Replacing
// the global operator new applies to this whole test binary; it only forwards to malloc.
namespace {
std::atomic<std::uint64_t> countedAllocations{0};
thread_local bool countAllocations = false;
void* allocate(std::size_t size) {
  if (countAllocations) countedAllocations.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}
}  // namespace
void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace std::chrono_literals;

constexpr auto kDeadline = 5s;

/// PipeWire's main loop, as the engine's would be; the handoffs' `closed` runs here.
class MainLoop {
public:
  MainLoop() {
    pw_init(nullptr, nullptr);
    loop_ = pw_main_loop_new(nullptr);
    REQUIRE(loop_ != nullptr);
  }
  ~MainLoop() {
    pw_main_loop_destroy(loop_);
    pw_deinit();
  }
  pw_loop* loop() const { return pw_main_loop_get_loop(loop_); }
  void quit() { pw_main_loop_quit(loop_); }

  /// Runs the main loop until quit() (from a callback); false if the deadline came first.
  bool runUntilQuit() {
    timedOut_ = false;
    spa_source* timer = pw_loop_add_timer(loop(), &MainLoop::onTimeout, this);
    timespec value{static_cast<time_t>(kDeadline.count()), 0};
    pw_loop_update_timer(loop(), timer, &value, nullptr, false);
    pw_main_loop_run(loop_);
    pw_loop_destroy_source(loop(), timer);
    return !timedOut_;
  }

  /// Dispatches what is already pending, without waiting.
  void dispatchPending() {
    pw_loop_enter(loop());
    pw_loop_iterate(loop(), 0);
    pw_loop_leave(loop());
  }

private:
  static void onTimeout(void* data, std::uint64_t /*expirations*/) {
    auto* self = static_cast<MainLoop*>(data);
    self->timedOut_ = true;
    self->quit();
  }
  pw_main_loop* loop_ = nullptr;
  bool timedOut_ = false;
};

/// A one-way gate a consumer waits at; opening it twice is harmless.
class Gate {
public:
  void open() {
    {
      std::lock_guard lock(mutex_);
      open_ = true;
    }
    opened_.notify_all();
  }
  void wait() {
    std::unique_lock lock(mutex_);
    opened_.wait(lock, [&] { return open_; });
  }

private:
  std::mutex mutex_;
  std::condition_variable opened_;
  bool open_ = false;
};

/// Opens the gate when the test ends, however it ends, so a failed assertion never leaves
/// the handoff's thread waiting while its handoff is destroyed.
struct OpenOnExit {
  Gate& gate;
  ~OpenOnExit() { gate.open(); }
};

/// Records everything it is handed, and the thread it was handed on.
class RecordingConsumer : public HandoffConsumer {
public:
  void consume(std::span<const float> interleaved, std::uint32_t frames) override {
    {
      std::lock_guard lock(mutex_);
      if (finishes_ > 0) consumedAfterFinish_ = true;
      samples_.insert(samples_.end(), interleaved.begin(), interleaved.end());
      frames_ += frames;
      threads_.push_back(std::this_thread::get_id());
      if (interleaved.size() != static_cast<std::size_t>(frames) * channels) badSpan_ = true;
    }
    changed_.notify_all();
  }
  void finish() override {
    std::lock_guard lock(mutex_);
    ++finishes_;
  }

  /// Waits until `frames` frames have arrived; false at the deadline.
  bool waitForFrames(std::uint64_t frames) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kDeadline, [&] { return frames_ >= frames; });
  }

  std::uint32_t channels = 2;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<float> samples_;
  std::uint64_t frames_ = 0;
  std::vector<std::thread::id> threads_;
  int finishes_ = 0;
  bool consumedAfterFinish_ = false;
  bool badSpan_ = false;
};

/// Planar test signal: frame n of channel c is n * channels + c, so the interleaved stream
/// counts up 0, 1, 2, ... exactly when frames arrive in order and channels in their order.
class Signal {
public:
  Signal(std::uint32_t channels, std::uint32_t frames)
      : planes_(channels, std::vector<float>(frames)), views_(channels) {
    for (std::uint32_t c = 0; c < channels; ++c) views_[c] = planes_[c];
  }
  /// Fills the next block, starting at frame `first`.
  std::span<const std::span<const float>> block(std::uint64_t first) {
    const auto channels = static_cast<std::uint32_t>(planes_.size());
    for (std::uint32_t c = 0; c < channels; ++c) {
      for (std::size_t f = 0; f < planes_[c].size(); ++f) {
        planes_[c][f] = static_cast<float>((first + f) * channels + c);
      }
    }
    return views_;
  }

private:
  std::vector<std::vector<float>> planes_;
  std::vector<std::span<const float>> views_;
};

/// Closes `handoff` and runs the main loop until `closed`; returns the dropped count.
std::uint64_t closeAndWait(MainLoop& main, Handoff& handoff) {
  std::uint64_t dropped = ~0ull;
  bool called = false;
  handoff.close([&](std::uint64_t droppedFrames) {
    dropped = droppedFrames;
    called = true;
    main.quit();
  });
  REQUIRE(main.runUntilQuit());
  REQUIRE(called);
  return dropped;
}

TEST_CASE("Frames from a writer thread arrive in order, interleaved, on the handoff thread",
          "[handoff]") {
  MainLoop main;
  ThreadHandoffs handoffs(main.loop());
  RecordingConsumer consumer;
  constexpr std::uint32_t kBlock = 64;
  constexpr std::uint32_t kBlocks = 100;
  // Room for every block, so nothing is dropped however the threads interleave.
  auto handoff = handoffs.make("a-node-with-a-long-name", 2, kBlock * kBlocks, consumer);

  // Catch's assertions belong to the test's thread, so the writer only records.
  std::thread::id writerThread;
  std::uint64_t written = 0;
  std::thread writer([&] {
    writerThread = std::this_thread::get_id();
    Signal signal(2, kBlock);
    for (std::uint32_t b = 0; b < kBlocks; ++b) {
      written += handoff->writer().write(signal.block(std::uint64_t{b} * kBlock), kBlock);
    }
  });
  writer.join();
  REQUIRE(written == kBlock * kBlocks);
  REQUIRE(consumer.waitForFrames(kBlock * kBlocks));
  REQUIRE(closeAndWait(main, *handoff) == 0);

  std::lock_guard lock(consumer.mutex_);
  REQUIRE(consumer.frames_ == kBlock * kBlocks);
  REQUIRE_FALSE(consumer.badSpan_);
  REQUIRE(consumer.samples_.size() == 2u * kBlock * kBlocks);
  for (std::size_t i = 0; i < consumer.samples_.size(); ++i) {
    if (consumer.samples_[i] != static_cast<float>(i)) FAIL("sample " << i << " is " << consumer.samples_[i]);
  }
  REQUIRE_FALSE(consumer.threads_.empty());
  const std::thread::id handoffThread = consumer.threads_.front();
  for (const auto& id : consumer.threads_) REQUIRE(id == handoffThread);
  REQUIRE(handoffThread != writerThread);
  REQUIRE(handoffThread != std::this_thread::get_id());
}

/// Blocks in its first consume() until released, so the ring stays full.
class StalledConsumer : public RecordingConsumer {
public:
  void consume(std::span<const float> interleaved, std::uint32_t frames) override {
    if (!entered_.exchange(true)) {
      {
        std::lock_guard lock(mutex_);
        stalled_ = true;
      }
      changed_.notify_all();
      release.wait();
    }
    RecordingConsumer::consume(interleaved, frames);
  }
  bool waitUntilStalled() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kDeadline, [&] { return stalled_; });
  }
  Gate release;

private:
  std::atomic<bool> entered_{false};
  bool stalled_ = false;
};

TEST_CASE("A stalled consumer makes write return short and closed reports the refused frames",
          "[handoff]") {
  MainLoop main;
  ThreadHandoffs handoffs(main.loop());
  StalledConsumer consumer;
  constexpr std::uint32_t kBlock = 100;
  auto handoff = handoffs.make("rec", 2, 256, consumer);
  OpenOnExit opener{consumer.release};
  Signal signal(2, kBlock);

  std::uint64_t accepted = handoff->writer().write(signal.block(0), kBlock);
  REQUIRE(accepted == kBlock);
  REQUIRE(consumer.waitUntilStalled());
  std::uint64_t refused = 0;
  bool shortWrite = false;
  for (int b = 1; b < 10; ++b) {
    const std::uint32_t got = handoff->writer().write(signal.block(accepted), kBlock);
    REQUIRE(got <= kBlock);
    if (got < kBlock) shortWrite = true;
    accepted += got;
    refused += kBlock - got;
  }
  REQUIRE(shortWrite);
  REQUIRE(refused > 0);
  consumer.release.open();
  REQUIRE(closeAndWait(main, *handoff) == refused);
  std::lock_guard lock(consumer.mutex_);
  REQUIRE(consumer.frames_ == accepted);
}

TEST_CASE("close drains the ring, finishes once after the last consume, then closed runs on "
          "the main loop", "[handoff]") {
  MainLoop main;
  ThreadHandoffs handoffs(main.loop());

  /// finish() blocks until released, so the test can see whether closed ran before it ended.
  class GatedFinish : public RecordingConsumer {
  public:
    void finish() override {
      RecordingConsumer::finish();
      {
        std::lock_guard lock(mutex_);
        finishEntered = true;
      }
      changed_.notify_all();
      release.wait();
      finishReturned.store(true);
    }
    bool waitForFinish() {
      std::unique_lock lock(mutex_);
      return changed_.wait_for(lock, kDeadline, [&] { return finishEntered; });
    }
    bool finishEntered = false;
    Gate release;
    std::atomic<bool> finishReturned{false};
  } consumer;

  constexpr std::uint32_t kBlock = 64;
  constexpr std::uint32_t kBlocks = 8;
  auto handoff = handoffs.make("rec", 2, kBlock * kBlocks, consumer);
  OpenOnExit opener{consumer.release};
  Signal signal(2, kBlock);
  for (std::uint32_t b = 0; b < kBlocks; ++b) {
    REQUIRE(handoff->writer().write(signal.block(std::uint64_t{b} * kBlock), kBlock) == kBlock);
  }

  // Closed at once, without waiting for the consumer: close itself drains the rest.
  bool closedCalled = false;
  bool finishedWhenClosed = false;
  std::uint64_t dropped = ~0ull;
  std::thread::id closedOn;
  handoff->close([&](std::uint64_t droppedFrames) {
    closedCalled = true;
    finishedWhenClosed = consumer.finishReturned.load();
    dropped = droppedFrames;
    closedOn = std::this_thread::get_id();
    main.quit();
  });
  REQUIRE(consumer.waitForFinish());
  // finish() has started and not returned: whatever the main loop has pending now must not
  // include `closed`.
  main.dispatchPending();
  REQUIRE_FALSE(closedCalled);
  consumer.release.open();
  REQUIRE(main.runUntilQuit());

  REQUIRE(closedCalled);
  REQUIRE(finishedWhenClosed);
  REQUIRE(closedOn == std::this_thread::get_id());
  REQUIRE(dropped == 0);
  std::lock_guard lock(consumer.mutex_);
  REQUIRE(consumer.finishes_ == 1);
  REQUIRE_FALSE(consumer.consumedAfterFinish_);
  REQUIRE(consumer.frames_ == kBlock * kBlocks);
  for (std::size_t i = 0; i < consumer.samples_.size(); ++i) {
    if (consumer.samples_[i] != static_cast<float>(i)) FAIL("sample " << i << " is " << consumer.samples_[i]);
  }
}

TEST_CASE("write allocates nothing", "[handoff]") {
  MainLoop main;
  ThreadHandoffs handoffs(main.loop());
  RecordingConsumer consumer;
  constexpr std::uint32_t kBlock = 64;
  constexpr std::uint32_t kBlocks = 50;
  auto handoff = handoffs.make("rec", 2, kBlock * kBlocks, consumer);
  std::uint64_t allocations = ~0ull;
  std::uint32_t written = 0;

  std::thread writer([&] {
    // The signal is made before counting starts; only write() is counted.
    std::vector<std::vector<float>> planes(2, std::vector<float>(kBlock, 0.25f));
    std::vector<std::span<const float>> views{planes[0], planes[1]};
    const std::uint64_t before = countedAllocations.load();
    countAllocations = true;
    for (std::uint32_t b = 0; b < kBlocks; ++b) written += handoff->writer().write(views, kBlock);
    countAllocations = false;
    allocations = countedAllocations.load() - before;
  });
  writer.join();
  REQUIRE(written == kBlock * kBlocks);
  REQUIRE(allocations == 0);
  REQUIRE(consumer.waitForFrames(kBlock * kBlocks));
  REQUIRE(closeAndWait(main, *handoff) == 0);
}

}  // namespace
