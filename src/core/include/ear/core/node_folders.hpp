// The interim seam of one node type, sendspin, until slice 3c (docs/design/node-primitives.md) gives it stores and
// the store service's diagnostics replace the boot log. Every other type's function takes no argument:
// its folders are resources (the recordings folder is the recording kind's adapter, RecordingRequest).
#pragma once
#include <memory>
namespace ear::folder {
class ConfinedFolder;
}
namespace ear {
class LogSink;
/// What sendspin's factory reads, held open from boot; main.cpp always gives both.
struct NodeFolders {
	std::shared_ptr<const folder::ConfinedFolder> config{}; // the graph file folder: each "sendspin" node's key file
	/// Where sendspin's factory reports what it finds at boot (docs/output-records.md#log). Not owned: main's outlives
	/// every type. A type that reports anything needs it and throws std::logic_error when it is null, as for a folder,
	/// so a finding is never dropped for want of a sink.
	LogSink* log = nullptr;
};
} // namespace ear
