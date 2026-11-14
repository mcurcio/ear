// A node's way out from its own threads (docs/writing-a-node-type.md#the-node-handle): notes, losses and log
// entries, each landing only on the configuration that made the handle. NodeHost implements it on each
// configuration's own channel, so a stale configuration's calls are dropped by construction, not by a generation the
// type keeps.
#pragma once
#include <string>
#include <utility>
#include "ear/core/graph_spec.hpp"
#include "ear/core/log.hpp"
namespace ear {
/// One configuration's handle (NodeOf::handle(), NodeContext::handle()). Called from any of the configuration's own
/// threads (the main loop, its workers, its sink consumers), never from the realtime thread (`process`): a call takes
/// brief locks (the note rate's, an executor's queue, a log sink's) and never waits on the main loop. It may be kept
/// and called until the configuration's teardown ended (a leaked configuration's for ever). What it carries lands only
/// on that configuration: once the configuration has ended (closed, failed or leaked), calls are dropped, so nothing
/// of an old configuration reaches a newer one; a `log` that races the end itself may still land its one line (it
/// checks, then writes, and the end does not wait for it). A stop and a start keep it; a restart, a change of settings
/// or a failure ends it, and the next configuration has its own.
class NodeHandle {
public:
	/// The node's name.
	virtual const NodeName& node() const noexcept = 0;
	/// A fact: the node cannot go on. It fails `start-failed: <detail>` before it first rested `running` or `stopped`,
	/// `lost: <detail>` after. Never held back by the note rate; ignored once the configuration's teardown began.
	virtual void lost(std::string detail) = 0;
	/// Free text for people, a `log` record (docs/output-records.md#log): the framework sets `node`, and an empty
	/// `source` becomes the type's name. Straight to the log sinks, unordered with notes and states; dropped once the
	/// configuration has ended (but for one racing the end).
	virtual void log(LogEntry entry) = 0;
	/// Until slice 5 splits notes into status and events: `node <n> says <text>` (a `note` record), on the main loop
	/// in the order said, so a note is ordered with the node's states. Never a state. Paced per configuration: 32 at
	/// once, then one a second; the next note that passes is preceded by `notes-dropped <n>`. Heard until the
	/// configuration has ended, so a goodbye said during its teardown is heard.
	virtual void say(std::string text) = 0;

	void debug(std::string text) { log(LogEntry{.level = LogLevel::debug, .source = {}, .text = std::move(text)}); }
	void info(std::string text) { log(LogEntry{.level = LogLevel::info, .source = {}, .text = std::move(text)}); }
	void warn(std::string text) { log(LogEntry{.level = LogLevel::warn, .source = {}, .text = std::move(text)}); }
	void error(std::string text) { log(LogEntry{.level = LogLevel::error, .source = {}, .text = std::move(text)}); }

protected:
	~NodeHandle() = default;
};
} // namespace ear
