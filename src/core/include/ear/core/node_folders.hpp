// The node catalogue contract's one factory argument (docs/design/batch2.md section 11's erratum of 2026-11-06).
#pragma once
#include <memory>
namespace ear::folder {
class ConfinedFolder;
}
namespace ear {
/// The folders a node type may need, held open from boot. A type reads only those it names; main.cpp always gives both.
struct NodeFolders {
	std::shared_ptr<const folder::ConfinedFolder> recordings{}; // the folder "file" nodes write into
	std::shared_ptr<const folder::ConfinedFolder> config{}; // the graph file folder: each "sendspin" node's key file
};
} // namespace ear
