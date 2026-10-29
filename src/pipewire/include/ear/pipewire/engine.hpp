// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in .claude/errata.md and a re-cut, never a local edit.
#pragma once
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
struct pw_loop; struct pw_context; struct pw_core;
namespace ear::pipewire {
struct EngineSettings {
  std::filesystem::path configFile;   // engine.conf; must not set default.clock.*
  std::uint32_t graphFrequency;       // -> default.clock.rate and the only allowed rate
  std::uint32_t blockSize;            // -> default.clock.quantum = min-quantum = max-quantum
};
struct EngineError { std::string detail; };
/// PipeWire's engine inside EAR: main loop, context, EAR's own internal connection. No socket
/// in M1 (core.daemon=false). Main thread only.
class Engine {
public:
  static std::expected<std::unique_ptr<Engine>, EngineError> create(const EngineSettings& settings);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  void run();                                      // runs the main loop until quit()
  void quit();                                     // from a main-loop callback
  void onStopSignal(std::function<void()> handler);   // SIGINT/SIGTERM, as main-loop events
  pw_loop* mainLoop() const noexcept;              // for this package's adapters only
  pw_context* context() const noexcept;
  pw_core* core() const noexcept;
private:
  struct Impl; std::unique_ptr<Impl> impl_;
  explicit Engine(std::unique_ptr<Impl> impl);
};
}
