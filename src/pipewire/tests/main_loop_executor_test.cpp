// MainLoopExecutor on the real engine's main loop (docs/design/batch2.md section 4.3; acceptance
// criterion E1(9)): post order per thread, the loop thread, never inline, and work left at
// destruction destroyed unrun. Every wait runs the main loop until the awaited work has run;
// the deadline only fails the test.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>

#include "catch2/catch.hpp"
#include "ear/pipewire/main_loop_executor.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;

std::unique_ptr<Engine> makeEngine() {
  for (const char* name : {"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE",
                           "DBUS_SESSION_BUS_ADDRESS", "DBUS_SYSTEM_BUS_ADDRESS"}) {
    unsetenv(name);
  }
  auto engine = Engine::create(EngineSettings{EAR_ENGINE_CONF, 48000, 256});
  if (!engine) FAIL("Engine::create: " << engine.error().detail);
  return std::move(*engine);
}

/// Runs the main loop until posted work calls engine.quit(); a timer only fails the test.
bool runUntilQuit(Engine& engine, std::chrono::seconds deadline = std::chrono::seconds(5)) {
  bool timedOut = false;
  std::pair<Engine*, bool*> state{&engine, &timedOut};
  spa_source* timer = pw_loop_add_timer(
      engine.mainLoop(),
      [](void* data, std::uint64_t) {
        auto* s = static_cast<std::pair<Engine*, bool*>*>(data);
        *s->second = true;
        s->first->quit();
      },
      &state);
  timespec value{static_cast<time_t>(deadline.count()), 0};
  pw_loop_update_timer(engine.mainLoop(), timer, &value, nullptr, false);
  engine.run();
  pw_loop_destroy_source(engine.mainLoop(), timer);
  return !timedOut;
}

/// Sets a flag when destroyed (so a test sees work destroyed unrun).
struct DestroyFlag {
  explicit DestroyFlag(std::shared_ptr<std::atomic<bool>> flag) : flag_(std::move(flag)) {}
  DestroyFlag(DestroyFlag&& other) noexcept : flag_(std::move(other.flag_)) {}
  ~DestroyFlag() {
    if (flag_) flag_->store(true);
  }
  std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace

TEST_CASE("Work posted from four threads runs on the main loop's thread in each thread's order",
          "[executor]") {
  auto engine = makeEngine();
  MainLoopExecutor executor(*engine);
  constexpr int kThreads = 4;
  constexpr int kPosts = 1000;
  const std::thread::id loopThread = std::this_thread::get_id();   // run() runs here
  // Touched only on the loop thread.
  std::array<std::vector<int>, kThreads> seen;
  int run = 0;
  int offLoop = 0;

  std::vector<std::thread> posters;
  for (int t = 0; t < kThreads; ++t) {
    posters.emplace_back([&, t] {
      for (int i = 0; i < kPosts; ++i) {
        executor.post([&, t, i] {
          if (std::this_thread::get_id() != loopThread) ++offLoop;
          seen[t].push_back(i);
          if (++run == kThreads * kPosts) engine->quit();
        });
      }
    });
  }
  const bool done = runUntilQuit(*engine);
  for (std::thread& poster : posters) poster.join();
  REQUIRE(done);
  CHECK(offLoop == 0);
  for (int t = 0; t < kThreads; ++t) {
    REQUIRE(seen[t].size() == kPosts);
    for (int i = 0; i < kPosts; ++i) {
      if (seen[t][i] != i) FAIL("thread " << t << ": post " << i << " ran as " << seen[t][i]);
    }
  }
}

TEST_CASE("Work posted from the main loop runs after post() returns, never inside it",
          "[executor]") {
  auto engine = makeEngine();
  MainLoopExecutor executor(*engine);
  bool returned = false;
  bool ranAfterReturn = false;
  bool ran = false;
  executor.post([&] {
    executor.post([&] {
      ran = true;
      ranAfterReturn = returned;
      engine->quit();
    });
    returned = true;
  });
  REQUIRE(runUntilQuit(*engine));
  REQUIRE(ran);
  CHECK(ranAfterReturn);
}

TEST_CASE("Work still queued when the executor is destroyed is destroyed unrun", "[executor]") {
  auto engine = makeEngine();
  auto destroyed = std::make_shared<std::atomic<bool>>(false);
  bool ran = false;
  {
    MainLoopExecutor executor(*engine);
    executor.post([&ran, flag = DestroyFlag(destroyed)] { ran = true; });
    CHECK_FALSE(destroyed->load());
  }
  CHECK(destroyed->load());
  CHECK_FALSE(ran);

  // The loop runs on without it: a later executor's work runs, the old one's never does.
  MainLoopExecutor next(*engine);
  next.post([&] { engine->quit(); });
  REQUIRE(runUntilQuit(*engine));
  CHECK_FALSE(ran);
}
