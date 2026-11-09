// The old SPI under the new machines (docs/writing-a-node-type.md#the-old-interface;
// docs/writing-a-node-type.md#the-code): sendspin's NodeTypeOf and hand-made test configs run through this. Private to
// ear_core: only NodeConfig::make's default makes one, and NodeHost drives it as any ErasedInstance. Goes with the old
// SPI in slice 3c.
#pragma once
#include <expected>
#include <functional>
#include <memory>

#include "ear/core/lifecycle.hpp"

namespace ear::detail {

class LegacyInstance final : public ErasedInstance {
public:
	/// `config` outlives the instance (teardown step 5 frees the config last).
	explicit LegacyInstance(const NodeConfig& config) noexcept : config_(config) {}
	/// config.start(cx.legacy): a failed StartResult is Configured::failed with its detail; else the code is handed to
	/// cx.create.use and the recipe kept for ready().
	Configured configure(InstanceContexts& cx) override;
	bool hasRecipe() const noexcept override { return static_cast<bool>(recipe_); }
	bool engineRefused() const noexcept override { return engineRefused_; }
	/// Runs the recipe (if any) and keeps the run object; its failure is returned.
	std::expected<void, FailureReason> ready(Ready& ready) override;
	/// RunObject::close, or closed() at once with none.
	void close(std::move_only_function<void()> closed) override;
	/// The run object the recipe built, while the instance lives; null before or with none (the type harness's).
	RunObject* runObject() const noexcept { return object_.get(); }

private:
	const NodeConfig& config_;
	RunRecipe recipe_;
	std::unique_ptr<RunObject> object_; // destroyed with the instance, after the code (teardown step 5)
	bool engineRefused_ = false;        // its start failed engineRefused
};

} // namespace ear::detail
