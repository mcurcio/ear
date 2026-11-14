#include "ear/core/loop.hpp"

namespace ear {
namespace {
thread_local bool strandHere = false; // the calling thread runs a strand's handler
} // namespace

std::string_view loopKindName(LoopKind kind) noexcept { return kind == LoopKind::strand ? "strand" : "dedicated"; }

std::string loopThreadName(std::string_view name, const NodeName& node) {
	std::string thread = "ear-" + std::string(name) + " " + node;
	if (thread.size() > 15) thread.resize(15);
	return thread;
}

bool onStrand() noexcept { return strandHere; }

StrandScope::StrandScope() noexcept : was_(strandHere) { strandHere = true; }

StrandScope::~StrandScope() { strandHere = was_; }

} // namespace ear
