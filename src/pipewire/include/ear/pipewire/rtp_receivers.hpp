#pragma once
#include "ear/core/companions.hpp"
#include "ear/pipewire/module_converters.hpp"
namespace ear::pipewire {
/// RTP receivers, the first module-run converter (docs/engine.md#rtp-receivers): one receiver = PipeWire's
/// libpipewire-module-rtp-source loaded into the engine's context with the RTP receiver's and RTP stream's rows of the
/// property table as its arguments (docs/engine.md#properties-ear-sets), every one built from the shape in
/// rtp_receivers.cpp (the one home of that form). Its node (Stream/Output/Audio, a pw_stream on the module's own
/// internal connection) has no node.group, never drives, and runs alone on the graph's driver until joined. Its ports:
/// "receive_AUX<i>". The module keeps the jitter buffer, the tracking of the sender's clock and the resampler.
/// Lifecycle: ModuleConverters'. RUNNING means its socket is bound (the module binds it when its stream starts, before
/// the node runs). A socket that cannot be bound gives no event on PipeWire 1.4.2: the node never runs, so it is never
/// linked or ready, and the EAR node stays starting until NodeHost's readiness deadline (5 s) fails it engine-refused
/// (docs/engine.md#rtp-receivers, docs/engine.md#readiness-deadlines).
using RtpReceivers = ModuleConverters<RtpReceiverShape>;
extern template class ModuleConverters<RtpReceiverShape>;
} // namespace ear::pipewire
