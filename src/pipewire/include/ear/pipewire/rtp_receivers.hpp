#pragma once
#include <memory>
#include "ear/core/companions.hpp"
#include "ear/pipewire/engine.hpp"
namespace ear::pipewire {
/// One receiver = PipeWire's libpipewire-module-rtp-source loaded into the engine's context
/// (pw_context_load_module) with the arguments docs/engine.md#rtp-receivers lists, every one
/// built here from the shape (the one home of that form): remote.name = internal (the module
/// finds no core in the context and would otherwise look for a daemon's socket), source.ip,
/// source.port, sess.media audio, sess.latency.msec, sess.ignore-ssrc = true, audio.format
/// S16BE or S24BE, audio.rate, audio.channels, audio.position AUX0.., node.name =
/// node.description = media.name = engineNode, and stream.props { node.always-process = true,
/// adapter.auto-port-config dsp }. Its node (Stream/Output/Audio, a pw_stream on the module's own
/// internal connection) has no node.group, never drives, and runs alone on the graph's driver
/// until joined. Its ports: "receive_AUX<i>".
/// Lifecycle: the one engine-companion lifecycle (engine_companions.hpp), with this kind's body: a
/// registry binding and a pw_core_sync first, so a node of the same name still going (a
/// restart's old receiver) is known as stale; then the module is loaded; the first node global
/// named engineNode that is not stale is this receiver's, bound for its info. RUNNING (the
/// module binds its socket when its stream starts, before the node runs) lets the channels be
/// linked; node ERROR, the node's global removed or the module destroyed fail it; a module that
/// will not load fails it (told later, never inside make()). A socket that cannot be bound
/// gives no event on PipeWire 1.4.2: the node never runs, so it is never linked or ready, and
/// the EAR node stays starting (docs/engine.md#rtp-receivers, finding g).
class RtpReceivers final : public RtpReceiverCompanions {
public:
	explicit RtpReceivers(Engine& engine); // engine outlives it
	std::unique_ptr<EngineCompanion> make(const CompanionTarget& target, const RtpReceiverShape& shape,
	                                      EngineCompanionEvents& events) override;

private:
	Engine& engine_;
};
} // namespace ear::pipewire
