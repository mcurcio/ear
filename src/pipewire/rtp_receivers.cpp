// RTP receivers through PipeWire's rtp-source module (docs/engine.md#rtp-receivers, rtp_receivers.hpp): the first
// module-run converter. What is RTP's own is here, as ModuleRun<RtpReceiverShape>: the module, its arguments, its
// ports and its words. The body that loads it and finds its node is every module-run converter's
// (module_converter_body.hpp), and the one engine-companion lifecycle (engine_companions.hpp) links it once it runs
// and tears it down. The module keeps the jitter buffer, the tracking of the sender's clock and the resampler.
#include "ear/pipewire/rtp_receivers.hpp"

#include <cstddef>
#include <string>

#include "engine_property_set.hpp"
#include "module_converter_body.hpp"

namespace ear::pipewire {
namespace {

/// The receiver's port for channel `channel` (from 0): a node.network stream's output ports are
/// "receive_<position>" (impl-port.c), and its positions are AUX0...
std::string channelPort(std::size_t channel) { return "receive_AUX" + std::to_string(channel); }

} // namespace

template <> struct ModuleRun<RtpReceiverShape> {
	static constexpr ConverterModule module{.kind = "RTP receiver",
	                                        .word = "rtp-source",
	                                        .body = "RtpReceiverBody",
	                                        .flow = ChannelFlow::nodeToBlock,
	                                        .nodePort = &channelPort};
	using Arguments = ModuleArguments<Home::rtpReceiver>;

	/// The module's arguments, each key from the property table (docs/engine.md#properties-ear-sets says why each): the
	/// one home of this form. Every value is a number, a closed enum, an RtpAddress or the engine node's name
	/// ([A-Za-z0-9_.-]), so no user text reaches PipeWire's parsers.
	static Arguments arguments(const NodeName& engineNode, const RtpReceiverShape& shape, std::size_t channels) {
		const std::string name = "\"" + engineNode + "\"";
		ModuleArguments<Home::rtpStream> stream;
		stream.add(property<Home::rtpStream>("node.always-process"), "true")
			.add(property<Home::rtpStream>("adapter.auto-port-config"),
		         "{ mode = dsp monitor = false control = false position = preserve }");
		Arguments arguments;
		arguments.add(property<Home::rtpReceiver>("remote.name"), "\"internal\"")
			.add(property<Home::rtpReceiver>("source.ip"), "\"" + shape.address.text() + "\"")
			.add(property<Home::rtpReceiver>("source.port"), std::to_string(shape.port))
			.add(property<Home::rtpReceiver>("sess.media"), "\"audio\"")
			.add(property<Home::rtpReceiver>("sess.latency.msec"), std::to_string(shape.latencyMs))
			.add(property<Home::rtpReceiver>("sess.ignore-ssrc"), "true")
			.add(property<Home::rtpReceiver>("audio.format"),
		         shape.encoding == RtpEncoding::l24 ? "\"S24BE\"" : "\"S16BE\"")
			.add(property<Home::rtpReceiver>("audio.rate"), std::to_string(shape.rate))
			.add(property<Home::rtpReceiver>("audio.channels"), std::to_string(channels))
			.add(property<Home::rtpReceiver>("audio.position"), auxPositions(channels))
			.add(property<Home::rtpReceiver>("node.name"), name)
			.add(property<Home::rtpReceiver>("node.description"), name)
			.add(property<Home::rtpReceiver>("media.name"), name)
			.add(property<Home::rtpReceiver>("stream.props"), stream);
		return arguments;
	}

	static pw_impl_module* load(pw_context* context, const Arguments& arguments) {
		return loadModule(context, "libpipewire-module-rtp-source", arguments);
	}
};

template class ModuleConverters<RtpReceiverShape>;

} // namespace ear::pipewire
