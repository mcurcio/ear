// The legacy adapter (legacy_instance.hpp) and NodeConfig's two defaults, start and make (node_type.hpp).
#include "legacy_instance.hpp"

#include <stdexcept>
#include <utility>

namespace ear {

StartResult NodeConfig::start(StartContext&) const { throw std::logic_error("a type on the new SPI has no start"); }

std::unique_ptr<ErasedInstance> NodeConfig::make(NodeContext&) const {
	return std::make_unique<detail::LegacyInstance>(*this);
}

namespace detail {

Configured LegacyInstance::configure(InstanceContexts& cx) {
	StartResult started = config_.start(cx.legacy);
	if (started.failed()) return Configured::failed(std::move(started.failure().detail));
	recipe_ = std::move(started.recipe());
	cx.create.use(std::move(started.code()));
	return Configured::applied();
}

std::expected<void, FailureReason> LegacyInstance::ready(Ready& ready) {
	if (!recipe_) return {};
	auto made = recipe_(ready);
	if (!made) return std::unexpected(std::move(made.error()));
	object_ = std::move(*made);
	return {};
}

void LegacyInstance::close(std::move_only_function<void()> closed) {
	if (object_) object_->close(std::move(closed));
	else closed();
}

} // namespace detail
} // namespace ear
