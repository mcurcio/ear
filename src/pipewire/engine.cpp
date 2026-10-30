// PipeWire's engine inside EAR (docs/design/m1-white-to-file.md section 1, "Engine facts";
// docs/design/batch2.md section 4.3). Slice E1 builds CAP_SYS_NICE and the scheduling report.
#include "ear/pipewire/engine.hpp"

#include <algorithm>
#include <csignal>
#include <string>
#include <utility>

#include <pthread.h>

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

namespace ear::pipewire {
namespace {

/// The two stop signals onStopSignal turns into main-loop events.
sigset_t stopSignals() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  return set;
}

/// The clock settings EngineSettings decides. pw_context_new reads them from its own
/// properties; engine.conf's context.properties would overwrite them, so it sets none.
pw_properties* contextProperties(const EngineSettings& settings) {
  const auto rate = std::to_string(settings.graphFrequency);
  const auto block = std::to_string(settings.blockSize);
  pw_properties* props = pw_properties_new(
      PW_KEY_CONFIG_NAME, settings.configFile.c_str(),
      "default.clock.rate", rate.c_str(),
      "default.clock.quantum", block.c_str(),
      "default.clock.min-quantum", block.c_str(),
      "default.clock.max-quantum", block.c_str(),
      // The engine rounds the quantum down to a power of two unless told not to, so a block
      // size such as 480 would otherwise run as 256.
      "clock.power-of-two-quantum", "false",
      nullptr);
  pw_properties_setf(props, "default.clock.allowed-rates", "[ %u ]", settings.graphFrequency);
  // The quantum is clamped to [floor, limit] (defaults 4 and 8192): widen them to hold it.
  pw_properties_setf(props, "default.clock.quantum-floor", "%u",
                     std::min(settings.blockSize, 4u));
  pw_properties_setf(props, "default.clock.quantum-limit", "%u",
                     std::max(settings.blockSize, 8192u));
  return props;
}

}  // namespace

struct Engine::Impl {
  sigset_t previousMask{};
  bool initialised = false;
  pw_main_loop* loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  std::function<void()> stopHandler;
  spa_source* sigint = nullptr;
  spa_source* sigterm = nullptr;

  ~Impl() {
    if (loop != nullptr) {
      if (sigint != nullptr) pw_loop_destroy_source(pw_main_loop_get_loop(loop), sigint);
      if (sigterm != nullptr) pw_loop_destroy_source(pw_main_loop_get_loop(loop), sigterm);
    }
    if (core != nullptr) pw_core_disconnect(core);
    if (context != nullptr) pw_context_destroy(context);
    if (loop != nullptr) pw_main_loop_destroy(loop);
    if (initialised) {
      pw_deinit();
      pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
    }
  }

  static void onSignal(void* data, int /*signalNumber*/) {
    auto* self = static_cast<Impl*>(data);
    if (self->stopHandler) self->stopHandler();
  }
};

std::expected<std::unique_ptr<Engine>, EngineError> Engine::create(const EngineSettings& settings) {
  auto impl = std::make_unique<Impl>();
  // The engine starts its data-loop thread in pw_context_new, and a stop signal sent to the
  // process goes to any thread that does not block it, where its default action ends the
  // process. So the stop signals are blocked here, before any engine thread exists, and every
  // thread started from this one later inherits that. onStopSignal reads them from a
  // signalfd, which also reports a signal that arrived before it was called.
  const sigset_t stop = stopSignals();
  pthread_sigmask(SIG_BLOCK, &stop, &impl->previousMask);
  pw_init(nullptr, nullptr);
  impl->initialised = true;

  impl->loop = pw_main_loop_new(nullptr);
  if (impl->loop == nullptr) {
    return std::unexpected(EngineError{"cannot make the main loop"});
  }
  impl->context =
      pw_context_new(pw_main_loop_get_loop(impl->loop), contextProperties(settings), 0);
  if (impl->context == nullptr) {
    return std::unexpected(EngineError{"cannot start the engine with " +
                                       settings.configFile.string() + ": " +
                                       spa_strerror(-errno)});
  }
  impl->core = pw_context_connect_self(impl->context, nullptr, 0);
  if (impl->core == nullptr) {
    return std::unexpected(EngineError{std::string("cannot connect to the engine: ") +
                                       spa_strerror(-errno)});
  }
  return std::unique_ptr<Engine>(new Engine(std::move(impl)));
}

Engine::Engine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Engine::~Engine() = default;

void Engine::run() { pw_main_loop_run(impl_->loop); }

void Engine::quit() { pw_main_loop_quit(impl_->loop); }

void Engine::onStopSignal(std::function<void()> handler) {
  impl_->stopHandler = std::move(handler);
  pw_loop* loop = pw_main_loop_get_loop(impl_->loop);
  if (impl_->sigint == nullptr) impl_->sigint = pw_loop_add_signal(loop, SIGINT, &Impl::onSignal, impl_.get());
  if (impl_->sigterm == nullptr) impl_->sigterm = pw_loop_add_signal(loop, SIGTERM, &Impl::onSignal, impl_.get());
}

Scheduling Engine::dataThreadScheduling() const { return {false, "unknown"}; }   // E1

pw_loop* Engine::mainLoop() const noexcept { return pw_main_loop_get_loop(impl_->loop); }

pw_context* Engine::context() const noexcept { return impl_->context; }

pw_core* Engine::core() const noexcept { return impl_->core; }

}  // namespace ear::pipewire
