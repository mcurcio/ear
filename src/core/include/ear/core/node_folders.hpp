// The interim seam of one node type, sendspin, until slice 3 of docs/design/node-framework.md gives it stores (7.1) and
// the store service's diagnostics replace the boot log (8.8's erratum). Every other type's function takes no argument:
// its folders are resources (7.3; the recordings folder is the recording kind's adapter, RecordingRequest).
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
	/// Where sendspin's factory reports what it finds at boot (docs/design/node-framework.md section 8.8, its erratum of
	/// slice 4). Not owned: main's outlives every type. A type that reports anything needs it and throws
	/// std::logic_error when it is null, as for a folder, so a finding is never dropped for want of a sink.
	LogSink* log = nullptr;
};
} // namespace ear
