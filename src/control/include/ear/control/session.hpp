// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include "ear/control/gateway.hpp"
#include "ear/core/levels.hpp"
// The protocol per connection (control thread only; docs/design/batch2.md sections 4.5 and 6).
namespace ear::control {
using Message = std::shared_ptr<const std::string>;   // encoded once, shared by every client
/// Implemented by the transport.
class Outbox {
public:
  /// A response or a graph notification, in order. The transport closes a connection whose
  /// queue is full (section 6).
  virtual void send(Message message) = 0;
  /// A complete levels snapshot. Replaces one not yet written (it holds every block, so
  /// nothing is lost).
  virtual void sendLevels(Message message) = 0;
protected:
  ~Outbox() = default;
};
class ControlHub;
class ControlSession {
public:
  ControlSession(ControlHub& hub, Outbox& out);   // attaches
  ~ControlSession();                              // detaches; later replies for it are dropped
  /// One text message. Its commands go to the gateway in one run(); `done` runs once all its
  /// responses are queued (the transport reads the next message only then).
  void received(std::string text, std::move_only_function<void()> done);
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
/// Sessions, subscriptions, the event stream and levels. Control thread only, construction
/// included. graph.subscribe: the session gets events after its snapshot, never before.
/// levels.subscribe is answered here, without the main loop.
class ControlHub final : public EventSink {
public:
  ControlHub(GraphGateway& graph, LevelReader& levels);   // calls graph.watch(*this)
  ~ControlHub();
  void graphChanged(GraphEvent event) override;   // encoded once, to subscribed sessions
  void levelsPublished();                         // latestLevels(), encode once, sendLevels
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
