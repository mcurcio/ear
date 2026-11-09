// RTP receivers through PipeWire's rtp-source module (docs/engine.md#rtp-receivers,
// rtp_receivers.hpp). A receiver is one kind of engine companion: its body here loads the module
// into the engine's context and finds the module's node through the registry; the one
// engine-companion lifecycle (engine_companions.hpp) links it once it runs and tears it down.
#include "ear/pipewire/rtp_receivers.hpp"

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
#include "engine_companions.hpp"
#include "engine_property_set.hpp"

namespace ear::pipewire {
namespace {

constexpr char moduleName[] = "libpipewire-module-rtp-source";
constexpr char kind[] = "RTP receiver"; // names it in failure details

/// The receiver's port for channel `channel` (from 0): a node.network stream's output ports are
/// "receive_<position>" (impl-port.c), and its positions are AUX0...
std::string channelPort(std::size_t channel) { return "receive_AUX" + std::to_string(channel); }

/// The module's arguments, each key from the property table (docs/engine.md#properties-ear-sets says why each): the one
/// home of this form. Every value is a number, a closed enum, an RtpAddress or the engine node's name
/// ([A-Za-z0-9_.-]), so no user text reaches PipeWire's parsers.
ModuleArguments moduleArguments(const NodeName& engineNode, const RtpReceiverShape& shape, std::size_t channels) {
	const std::string name = "\"" + engineNode + "\"";
	ModuleArguments stream;
	stream.add(property(Home::rtpStream, "node.always-process"), "true")
		.add(property(Home::rtpStream, "adapter.auto-port-config"),
	         "{ mode = dsp monitor = false control = false position = preserve }");
	ModuleArguments arguments;
	arguments.add(property(Home::rtpReceiver, "remote.name"), "\"internal\"")
		.add(property(Home::rtpReceiver, "source.ip"), "\"" + shape.address.text() + "\"")
		.add(property(Home::rtpReceiver, "source.port"), std::to_string(shape.port))
		.add(property(Home::rtpReceiver, "sess.media"), "\"audio\"")
		.add(property(Home::rtpReceiver, "sess.latency.msec"), std::to_string(shape.latencyMs))
		.add(property(Home::rtpReceiver, "sess.ignore-ssrc"), "true")
		.add(property(Home::rtpReceiver, "audio.format"),
	         shape.encoding == RtpEncoding::l24 ? "\"S24BE\"" : "\"S16BE\"")
		.add(property(Home::rtpReceiver, "audio.rate"), std::to_string(shape.rate))
		.add(property(Home::rtpReceiver, "audio.channels"), std::to_string(channels))
		.add(property(Home::rtpReceiver, "audio.position"), auxPositions(channels))
		.add(property(Home::rtpReceiver, "node.name"), name)
		.add(property(Home::rtpReceiver, "node.description"), name)
		.add(property(Home::rtpReceiver, "media.name"), name)
		.add(property(Home::rtpReceiver, "stream.props"), stream);
	return arguments;
}

/// One receiver's engine node: the rtp-source module loaded into the engine's context, its
/// node found through a registry binding on EAR's connection.
///   1. A registry and a pw_core_sync: node globals named engineNode announced before the sync's
///      done are stale (a restart's old receiver, still going); a stale id is forgotten when its
///      global goes, as the engine may give the id again.
///   2. On the done: the module is loaded. The first node global named engineNode that is not
///      stale is this receiver's; it is bound for its info.
///   3. Its info RUNNING (its socket is bound, docs/engine.md#rtp-receivers, finding g): running.
class RtpReceiverBody final : public CompanionBody {
public:
	RtpReceiverBody(Engine& engine, NodeName engineNode, ModuleArguments arguments, CompanionBodyEvents& events)
		: engine_(engine), engineNode_(std::move(engineNode)), arguments_(std::move(arguments)), events_(events) {
		registry_ = pw_core_get_registry(engine_.core(), PW_VERSION_REGISTRY, 0);
		if (registry_ == nullptr) {
			events_.failed(std::string("cannot watch the engine for the RTP receiver: ") + spa_strerror(-errno));
			return;
		}
		listen<RegistryCallbacks<RtpReceiverBody>{.global = guarded<&RtpReceiverBody::onGlobal>,
		                                          .globalRemove = guarded<&RtpReceiverBody::onGlobalRemove>}>(
			registry_, registryHook_, this);
		listen<CoreCallbacks<RtpReceiverBody>{.done = guarded<&RtpReceiverBody::onCoreDone>}>(engine_.core(), coreHook_,
		                                                                                      this);
		coreListening_ = true;
		// Without the sync's done there is no telling stale nodes from this receiver's: a failure.
		syncSeq_ = pw_core_sync(engine_.core(), PW_ID_CORE, 0);
		if (syncSeq_ < 0) events_.failed(std::string("cannot sync with the engine: ") + spa_strerror(syncSeq_));
	}

	~RtpReceiverBody() override {
		if (moduleListening_) spa_hook_remove(&moduleHook_);
		unbindNode();
		if (coreListening_) spa_hook_remove(&coreHook_);
		if (registry_ != nullptr) {
			spa_hook_remove(&registryHook_);
			pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
		}
		if (module_ != nullptr) pw_impl_module_destroy(module_);
	}

	RtpReceiverBody(const RtpReceiverBody&) = delete;
	RtpReceiverBody& operator=(const RtpReceiverBody&) = delete;

private:
	// 1 -> 2: every node of this name the engine had before the sync is stale.
	void onCoreDone(std::uint32_t id, int seq) {
		if (id != PW_ID_CORE || seq != syncSeq_ || loading_) return;
		coreListening_ = false;
		spa_hook_remove(&coreHook_);
		load();
	}

	void load() {
		loading_ = true;
		module_ = loadModule(engine_.context(), moduleName, arguments_);
		if (module_ == nullptr) {
			events_.failed(std::string("cannot load PipeWire's rtp-source module: ") + std::strerror(errno));
			return;
		}
		listen<ModuleCallbacks<RtpReceiverBody>{.destroy = guarded<&RtpReceiverBody::onModuleDestroy>}>(
			module_, moduleHook_, this);
		moduleListening_ = true;
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
			events_.failed("the engine removed the RTP receiver");
		}
	}

	void bindNode(std::uint32_t id) {
		node_ = static_cast<pw_proxy*>(pw_registry_bind(registry_, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
		if (node_ == nullptr) {
			events_.failed(std::string("cannot watch the RTP receiver: ") + spa_strerror(-errno));
			return;
		}
		nodeId_ = id;
		listen<NodeCallbacks<RtpReceiverBody>{.info = guarded<&RtpReceiverBody::onInfo>}>(node_, nodeHook_, this);
	}

	void unbindNode() {
		if (node_ == nullptr) return;
		spa_hook_remove(&nodeHook_);
		pw_proxy_destroy(node_);
		node_ = nullptr;
	}

	// 3: RUNNING lets the channels be linked, by the node's global id; ERROR fails it.
	void onInfo(const pw_node_info* info) { tellNodeState(*info, kind, nodeId_, events_); }

	void onModuleDestroy() {
		spa_hook_remove(&moduleHook_);
		moduleListening_ = false;
		module_ = nullptr;
		events_.failed("the engine removed the RTP receiver's module");
	}

	Engine& engine_;
	NodeName engineNode_;
	ModuleArguments arguments_;
	CompanionBodyEvents& events_;
	pw_registry* registry_ = nullptr;
	spa_hook registryHook_{};
	spa_hook coreHook_{};
	bool coreListening_ = false;
	int syncSeq_ = 0;
	bool loading_ = false;          // the sync is done: a node of this name now is ours
	std::set<std::uint32_t> stale_; // node ids of this name from before the sync
	pw_impl_module* module_ = nullptr;
	spa_hook moduleHook_{};
	bool moduleListening_ = false;
	pw_proxy* node_ = nullptr;
	std::uint32_t nodeId_ = SPA_ID_INVALID;
	spa_hook nodeHook_{};
};

} // namespace

RtpReceivers::RtpReceivers(Engine& engine) : engine_(engine) {}

std::unique_ptr<EngineCompanion> RtpReceivers::make(const CompanionTarget& target, const RtpReceiverShape& shape,
                                                    EngineCompanionEvents& events) {
	std::vector<PortName> receiverPorts;
	for (std::size_t channel = 0; channel < target.blockPorts.size(); ++channel) {
		receiverPorts.push_back(channelPort(channel));
	}
	return joinEngineCompanion(engine_,
	                           CompanionJoin{.kind = kind,
	                                         .blockId = target.blockId,
	                                         .flow = ChannelFlow::nodeToBlock,
	                                         .nodePorts = std::move(receiverPorts),
	                                         .blockPorts = target.blockPorts},
	                           events, [&](CompanionBodyEvents& body) -> std::unique_ptr<CompanionBody> {
								   return std::make_unique<RtpReceiverBody>(
									   engine_, target.engineNode,
									   moduleArguments(target.engineNode, shape, target.blockPorts.size()), body);
							   });
}

} // namespace ear::pipewire
