// The node catalogue contract's one factory argument (docs/design/batch2.md section 11's erratum of 2026-11-06).
#pragma once
#include <memory>
namespace ear::folder {
class ConfinedFolder;
}
namespace ear {
class LogSink;
/// The folders a node type may need, held open from boot. A type reads only those it names; main.cpp always gives both.
struct NodeFolders {
	std::shared_ptr<const folder::ConfinedFolder> recordings{}; // the folder "file" nodes write into
	std::shared_ptr<const folder::ConfinedFolder> config{}; // the graph file folder: each "sendspin" node's key file
	/// Where a type's factory reports what it finds at boot (docs/design/node-framework.md section 8.8, until the
	/// store service's diagnostics replace it, 7.1; its erratum of slice 4). Not owned: main's outlives every type. A
	/// type that reports anything needs it and throws std::logic_error when it is null, as for a folder, so a finding
	/// is never dropped for want of a sink.
	LogSink* log = nullptr;
};
} // namespace ear
