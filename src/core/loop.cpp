#include "ear/core/loop.hpp"

namespace ear {
namespace {
thread_local bool strandHere = false;   // the calling thread runs a strand's handler
thread_local bool mainLoopHere = false; // the calling thread is the main loop's
} // namespace

std::string_view loopKindName(LoopKind kind) noexcept { return kind == LoopKind::strand ? "strand" : "dedicated"; }

std::string loopThreadName(std::string_view name, const NodeName& node) {
	std::string thread = "ear-" + std::string(name) + " " + node;
	if (thread.size() > 15) thread.resize(15);
	return thread;
}

bool onStrand() noexcept { return strandHere; }

bool onMainLoop() noexcept { return mainLoopHere; }

void markMainLoopThread() noexcept { mainLoopHere = true; }

MainLoopMark::MainLoopMark(bool here) noexcept : was_(mainLoopHere) { mainLoopHere = here; }

MainLoopMark::~MainLoopMark() { mainLoopHere = was_; }

StrandScope::StrandScope() noexcept : was_(strandHere) { strandHere = true; }

StrandScope::~StrandScope() { strandHere = was_; }

} // namespace ear
