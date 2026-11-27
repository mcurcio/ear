// The two interfaces ear_control declares at its edge: where every request batch goes (the graph file keeper implements
// it), and who is told of each change to the graph file (the gateway and the status log). WallClock lives here, the
// edge both StatusLog and the gateway include (docs/control-port.md#node-health).
#pragma once
#include <chrono>
#include <functional>
#include <string>
#include <vector>
#include "ear/control/commands.hpp"
#include "ear/control/gestures_wire.hpp"
#include "ear/control/graph_file_state.hpp"
namespace ear::control {
/// What stamps an instant on the wire (`since`) and on standard output (`t`): the system clock, or a test's.
using WallClock = std::function<std::chrono::system_clock::time_point()>;
using BatchReply = std::move_only_function<void(std::vector<CommandResult>)>; // called once, on the main loop
/// Where every request batch goes (the graph file keeper implements it).
class GraphRequests {
public:
	/// Any thread; never runs anything inline. Runs `commands` in order, each as its own step, the
	/// batch whole before any later batch or disk change. Consecutive reads (GetGraph, GetGraphFile)
	/// are answered in one main-loop turn. `reply` is called on the main loop in the turn of the
	/// batch's last step, after every event the batch caused was told.
	virtual void submit(std::vector<Command> commands, BatchReply reply) = 0;

protected:
	~GraphRequests() = default;
};
/// Where gesture requests go on the main loop (feature gestures, docs/control-port.md#gestures): the graph file keeper
/// implements them over Gestures (docs/architecture.md#gestures). Main loop only; each answers with what the request is
/// answered with.
class GestureRequests {
public:
	virtual GestureAnswer begin(SessionToken session, const BeginGesture& begin) = 0;
	virtual void move(SessionToken session, const MoveGesture& move) = 0;
	/// `reply` runs once on the main loop: once the end committed, after its events, or at once when it is refused.
	virtual void end(SessionToken session, const EndGesture& end,
	                 std::move_only_function<void(GestureAnswer)> reply) = 0;
	virtual GestureAnswer cancel(SessionToken session, const CancelGesture& cancel) = 0;
	/// A session closed and what it queued has run: its gestures are released.
	virtual void sessionClosed(SessionToken session) = 0;

protected:
	~GestureRequests() = default;
};
/// Told on the main loop, before the node and route events a change causes.
class GraphFileListener {
public:
	virtual void graphFileChanged(const GraphFileChanged& change) = 0;
	virtual void graphFileWritten(const std::string& version) = 0; // status line only; not on the wire
protected:
	~GraphFileListener() = default;
};
/// Who fills in what a graph read cannot know: each node's `since`, when its phase began (the gateway's).
/// Main loop, at the read itself, so a snapshot's `since` is the one its
/// phase had when the graph was read, whatever the batch does after it.
class SnapshotStamp {
public:
	virtual void stamp(GraphView& view) const = 0;

protected:
	~SnapshotStamp() = default;
};
} // namespace ear::control
