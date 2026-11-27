#pragma once
#include <functional>
#include <memory>
#include <string>
#include "ear/control/gateway.hpp"
#include "ear/control/type_schemas.hpp"
#include "ear/core/levels.hpp"
// The protocol per connection (control thread only; docs/control-port.md).
namespace ear::control {
using Message = std::shared_ptr<const std::string>; // encoded once, shared by every client
/// The most messages a session holds read and not yet answered, the running one among them
/// (docs/control-port.md#per-client-limits-and-stopping).
inline constexpr std::size_t sessionFifoBound = 16;
/// Implemented by the transport.
class Outbox {
public:
	/// A response or a graph notification, in order. The transport closes a connection whose
	/// queue is full.
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
	ControlSession(ControlHub& hub, Outbox& out); // attaches
	~ControlSession();                            // detaches; later replies for it are dropped
	/// One text message, read as it arrived. It joins the session's FIFO, whose messages run one at a time in order,
	/// each one's commands in one gateway run(); `done` runs once the FIFO has room for another (at once, or once one
	/// is answered while it holds sessionFifoBound): the transport reads the next message only then. A session
	/// destroyed with messages in its FIFO still runs them in order, their replies dropped.
	void received(std::string text, std::move_only_function<void()> done);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
/// Sessions, subscriptions, the event stream and levels. Control thread only, construction
/// included. graph.subscribe: the session gets graph events after its snapshot, never before;
/// graphFile.subscribe: graphFile.changed after its snapshot, never before. A batch's subscribe
/// snapshots run after its other commands, in request order. levels.subscribe and types.get are answered here,
/// without the main loop; every Graph a session sends carries `types`' hash as typeSchemaHash.
class ControlHub final : public EventSink {
public:
	ControlHub(GraphGateway& graph, LevelReader& levels, TypeSchemas types = {}); // calls graph.watch(*this)
	~ControlHub();
	void graphChanged(GraphEvent event) override; // encoded once, to graph-subscribed sessions
	/// Encoded once (encodeGraphFileChanged), to graphFile-subscribed sessions.
	void graphFileChanged(GraphFileChanged change) override;
	void levelsPublished(); // latestLevels(), encode once, sendLevels
private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace ear::control
