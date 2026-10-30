// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "ear/control/gateway.hpp"
#include "ear/core/levels.hpp"
#include "ear/server/control_thread.hpp"
// No Boost type in this header: Beast and Asio stay private to ear_server.
namespace ear::server {
struct ControlPortSettings {
  std::string address;                     // --control-address, default "0.0.0.0"
  std::uint16_t port;                      // --control-port, default 7777; 0: any free port
  std::filesystem::path webRoot;           // --web-root, default <directory of ear>/web
  std::vector<std::string> allowedHosts;   // --allowed-host (repeatable): DNS names Host may use
};
struct ControlPortError { std::string detail; };
/// Realtime-safe bell: a non-blocking eventfd, rung at most once until the control thread
/// answers it. Made before the port blocks, which ring it.
class LevelBell final : public LevelWake {
public:
  LevelBell();                                  // throws std::system_error
  ~LevelBell();
  void levelsPublished() noexcept override;
  int fd() const noexcept;
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
/// Port 7777 on the control thread. Every request: its Host's name must be an IP literal,
/// "localhost" or one of allowedHosts (case-insensitive), else 403 (DNS rebinding: a name the
/// operator did not give cannot reach the port). GET/HEAD files from webRoot ("/" is
/// index.html; ".." refused; 404; other methods 405). WebSocket JSON-RPC at /rpc: Origin absent
/// or its host[:port] equal to Host's, else 403 without upgrade. Max message 64 KiB.
class ControlPort final {
public:
  /// Binds and listens, then makes the hub and starts accepting on `thread`. All arguments
  /// outlive the port.
  static std::expected<std::unique_ptr<ControlPort>, ControlPortError> start(
      const ControlPortSettings& settings, ControlThread& thread, control::GraphGateway& graph,
      LevelReader& levels, LevelBell& bell);
  ~ControlPort();                           // stop()
  std::uint16_t port() const noexcept;      // the bound port
  /// Any thread but the control thread; idempotent. On the control thread: closes the listener
  /// and every connection (WebSocket close 1001), destroys the hub; waits for that.
  void stop();
private:
  struct Impl; std::unique_ptr<Impl> impl_;
  explicit ControlPort(std::unique_ptr<Impl> impl);
};
}
