#include "completions.hpp"

#include <utility>

namespace ear::detail {

void Completions::post(std::move_only_function<void()> work) {
	// The check is made where the work lands, on the main thread that detaches, so a detach between this post and its
	// landing drops it.
	mainLoop_.post([self = shared_from_this(), work = std::move(work)]() mutable {
		if (self->attached()) work();
	});
}

} // namespace ear::detail
