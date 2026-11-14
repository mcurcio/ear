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

void Completions::postErased(std::shared_ptr<void> held, IoExecutor& io, std::string release,
                             std::move_only_function<void(void*)> landed) {
	mainLoop_.post([self = shared_from_this(), held = std::move(held), io = &io, release = std::move(release),
	                landed = std::move(landed)]() mutable {
		if (self->attached()) {
			landed(held.get());
			return;
		}
		landed = nullptr; // so nothing of the job's is left here once the handle is handed over
		io->post(std::move(release), [held = std::move(held)]() mutable { held.reset(); });
	});
}

} // namespace ear::detail
