// The module-run converter's body and adapter (docs/engine.md#module-run-converters), shared by every module-run
// shape: it loads the shape's module into the engine's context and finds the module's node through the registry; the
// one engine-companion lifecycle (engine_companions.hpp) links it once it runs and tears it down. A module says only
// what is its own (converter_modules.hpp). Private to ear_pipewire: its sources and tests include it by this path; no
// public header does. A shape's adapter instantiates ModuleConverters<Shape> in the source that defines its
// ModuleRun<Shape> (RtpReceivers: rtp_receivers.cpp).
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pipewire/impl.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include "callback_boundary.hpp"
#include "converter_modules.hpp"
#include "ear/pipewire/module_converters.hpp"
#include "engine_companions.hpp"

namespace ear::pipewire {

/// One module-run converter's engine node: the shape's module loaded into the engine's context, its node found through
/// a registry binding on EAR's connection.
///   1. A registry and a pw_core_sync: node globals named engineNode announced before the sync's done are stale (a
///      restart's old node, still going); a stale id is forgotten when its global goes, as the engine may give the id
///      again.
///   2. On the done: the module is loaded. The first node global named engineNode that is not stale is this
///      converter's; it is bound for its info.
///   3. Its info RUNNING (for RTP, its socket is bound: docs/engine.md#rtp-receivers): running.
/// A template only so that the boundary names its callbacks by the module's body (NamesItself): a fault report says
/// which module's converter threw, as it did before the body was shared.
template <class Shape>
	requires DescribedModuleRun<Shape>
class ModuleConverterBody final : public CompanionBody, public FaultOwner {
	using Run = ModuleRun<Shape>;

public:
	static constexpr std::string_view boundaryName = Run::module.body;

	ModuleConverterBody(Engine& engine, NodeName engineNode, typename Run::Arguments arguments,
	                    CompanionBodyEvents& events)
		: engine_(engine), engineNode_(std::move(engineNode)), arguments_(std::move(arguments)), events_(events) {
		// The registry and each listener are held by members declared in this order, so a throw below unwinds them,
		// listeners first (docs/architecture.md#faults-by-owner).
		registry_.reset(pw_core_get_registry(engine_.core(), PW_VERSION_REGISTRY, 0));
		if (registry_ == nullptr) {
			const int error = errno;
			events_.failed("cannot watch the engine for the " + kind() + ": " + spa_strerror(-error));
			return;
		}
		listen<RegistryCallbacks<ModuleConverterBody>{.global = guarded<&ModuleConverterBody::onGlobal>,
		                                              .globalRemove = guarded<&ModuleConverterBody::onGlobalRemove>}>(
			registry_.get(), registryListener_, this);
		listen<CoreCallbacks<ModuleConverterBody>{.done = guarded<&ModuleConverterBody::onCoreDone>}>(
			engine_.core(), coreListener_, this);
		// Without the sync's done there is no telling stale nodes from this converter's: a failure.
		syncSeq_ = pw_core_sync(engine_.core(), PW_ID_CORE, 0);
		if (syncSeq_ < 0) events_.failed(std::string("cannot sync with the engine: ") + spa_strerror(syncSeq_));
	}

	~ModuleConverterBody() override {
		moduleListener_.remove();
		unbindNode();
		coreListener_.remove();
		registryListener_.remove();
		registry_.reset();
		if (module_ != nullptr) pw_impl_module_destroy(module_);
	}

	ModuleConverterBody(const ModuleConverterBody&) = delete;
	ModuleConverterBody& operator=(const ModuleConverterBody&) = delete;

	// A throw out of the body's own callback is its bug: the lifecycle gives it up and fails its node lost
	// (docs/architecture.md#faults-by-owner).
	std::string_view faultNode() const noexcept override { return events_.faultNode(); }
	void faulted(std::string_view callback, std::string_view detail) override { events_.faulted(callback, detail); }

private:
	static std::string kind() { return std::string(Run::module.kind); }

	// 1 -> 2: every node of this name the engine had before the sync is stale.
	void onCoreDone(std::uint32_t id, int seq) {
		if (id != PW_ID_CORE || seq != syncSeq_ || loading_) return;
		coreListener_.remove();
		load();
	}

	void load() {
		loading_ = true;
		module_ = Run::load(engine_.context(), arguments_);
		if (module_ == nullptr) {
			const int error = errno;
			events_.failed("cannot load PipeWire's " + std::string(Run::module.word) +
			               " module: " + std::strerror(error));
			return;
		}
		listen<ModuleCallbacks<ModuleConverterBody>{.destroy = guarded<&ModuleConverterBody::onModuleDestroy>}>(
			module_, moduleListener_, this);
	}

	void onGlobal(std::uint32_t id, std::uint32_t /*permissions*/, const char* type, std::uint32_t /*version*/,
	              const spa_dict* props) {
		if (props == nullptr || std::string_view(type) != PW_TYPE_INTERFACE_Node) return;
		const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
		if (name == nullptr || engineNode_ != name) return;
		if (!loading_) {
			stale_.insert(id);
		} else if (node_ == nullptr && !stale_.contains(id)) {
			bindNode(id);
		}
	}

	void onGlobalRemove(std::uint32_t id) {
		stale_.erase(id);
		if (node_ != nullptr && id == nodeId_) {
			unbindNode();
			events_.failed("the engine removed the " + kind());
		}
	}

	void bindNode(std::uint32_t id) {
		node_.reset(
			static_cast<pw_proxy*>(pw_registry_bind(registry_.get(), id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0)));
		if (node_ == nullptr) {
			const int error = errno;
			events_.failed("cannot watch the " + kind() + ": " + spa_strerror(-error));
			return;
		}
		nodeId_ = id;
		listen<NodeCallbacks<ModuleConverterBody>{.info = guarded<&ModuleConverterBody::onInfo>}>(node_.get(),
		                                                                                          nodeListener_, this);
	}

	void unbindNode() {
		nodeListener_.remove();
		node_.reset();
	}

	// 3: RUNNING lets the channels be linked, by the node's global id; ERROR fails it.
	void onInfo(const pw_node_info* info) { tellNodeState(*info, kind(), nodeId_, events_); }

	void onModuleDestroy() {
		moduleListener_.remove();
		module_ = nullptr;
		events_.failed("the engine removed the " + kind() + "'s module");
	}

	Engine& engine_;
	NodeName engineNode_;
	typename Run::Arguments arguments_;
	CompanionBodyEvents& events_;
	OwnedRegistry registry_;
	Listener registryListener_;
	Listener coreListener_;
	int syncSeq_ = 0;
	bool loading_ = false;          // the sync is done: a node of this name now is ours
	std::set<std::uint32_t> stale_; // node ids of this name from before the sync
	pw_impl_module* module_ = nullptr;
	Listener moduleListener_;
	OwnedProxy node_;
	std::uint32_t nodeId_ = SPA_ID_INVALID;
	Listener nodeListener_;
};

template <class Shape> std::unique_ptr<EngineCompanion>
ModuleConverters<Shape>::make(const CompanionTarget& target, const Shape& shape, EngineCompanionEvents& events) {
	using Run = ModuleRun<Shape>;
	std::vector<PortName> nodePorts;
	for (std::size_t channel = 0; channel < target.blockPorts.size(); ++channel)
		nodePorts.push_back(Run::module.nodePort(channel));
	return joinEngineCompanion(engine_,
	                           CompanionJoin{.kind = std::string(Run::module.kind),
	                                         .node = target.blockNode,
	                                         .blockId = target.blockId,
	                                         .flow = Run::module.flow,
	                                         .nodePorts = std::move(nodePorts),
	                                         .blockPorts = target.blockPorts},
	                           events, [&](CompanionBodyEvents& body) -> std::unique_ptr<CompanionBody> {
								   return std::make_unique<ModuleConverterBody<Shape>>(
									   engine_, target.engineNode,
									   Run::arguments(target.engineNode, shape, target.blockPorts.size()), body);
							   });
}

} // namespace ear::pipewire
