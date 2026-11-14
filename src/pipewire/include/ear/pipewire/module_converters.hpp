#pragma once
#include <memory>
#include "ear/core/companions.hpp"
#include "ear/pipewire/engine.hpp"
namespace ear::pipewire {
/// Converter companions in module-run mode (docs/engine.md#module-run-converters): one per companion, a PipeWire module
/// loaded into the engine's context (pw_context_load_module) that owns the stream, its buffer, its resampler and the
/// loop that steers it; ear loads it, finds its node, links it, carries its latency (moduleRunDelay, NodeHost's plan)
/// and judges its life. What a module is (its name, arguments, ports and words) is its shape's ModuleRun<Shape>
/// (converter_modules.hpp), defined beside the shape's adapter, which instantiates this template (RtpReceivers, in
/// rtp_receivers.cpp).
/// Lifecycle: the one engine-companion lifecycle (engine_companions.hpp), with the module-run body
/// (module_converter_body.hpp): a registry binding and a pw_core_sync first, so a node of the same name still going (a
/// restart's old one) is known as stale; then the module is loaded; the first node global named engineNode that is not
/// stale is this companion's, bound for its info. RUNNING lets the channels be linked; node ERROR, the node's global
/// removed or the module destroyed fail it; a module that will not load fails it (told later, never inside make()). A
/// node that never runs tells nothing: NodeHost's readiness deadline for the shape's kind fails it
/// (docs/engine.md#readiness-deadlines). It never sets a property that forces or locks the graph's quantum or rate
/// (docs/engine.md#the-quantum-guard).
template <class Shape> class ModuleConverters final : public EngineCompanionAdapter<Shape> {
public:
	explicit ModuleConverters(Engine& engine) : engine_(engine) {} // engine outlives it
	std::unique_ptr<EngineCompanion> make(const CompanionTarget& target, const Shape& shape,
	                                      EngineCompanionEvents& events) override;

private:
	Engine& engine_;
};
} // namespace ear::pipewire
