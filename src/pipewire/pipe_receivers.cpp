// Pipe receivers through PipeWire's pipe-tunnel module (docs/engine.md#pipe-receivers, pipe_receivers.hpp): the second
// module-run converter. What is the pipe module's own is here, as ModuleRun<PipeReceiverShape>: the module, its
// arguments, its ports and its words. The body that loads it and finds its node is every module-run converter's
// (module_converter_body.hpp), and the one engine-companion lifecycle (engine_companions.hpp) links it once it runs
// and tears it down. The module makes the FIFO, keeps the ring, tracks the writer's pace and resamples.
#include "ear/pipewire/pipe_receivers.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

#include "engine_property_set.hpp"
#include "module_converter_body.hpp"

namespace ear::pipewire {
namespace {

/// The receiver's port for channel `channel` (from 0): an Audio/Source stream that is not node.network names its
/// output ports "capture_<position>" (impl-port.c), and its positions are AUX0...
std::string capturePort(std::size_t channel) { return "capture_AUX" + std::to_string(channel); }

/// The FIFO's path, quoted for the module's parser. It is ear's own (runtimePath beneath the runtime folder), so a
/// character outside [A-Za-z0-9_./-] is ear's bug, never a user's text reaching PipeWire's parser.
std::string quotedFifo(const std::string& fifo) {
	if (fifo.empty() || fifo.front() != '/') throw std::logic_error("a pipe receiver's FIFO is an absolute path");
	for (const char c : fifo) {
		const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
		                   c == '.' || c == '/' || c == '-';
		if (!plain) throw std::logic_error("a pipe receiver's FIFO holds a character outside [A-Za-z0-9_./-]: " + fifo);
	}
	return "\"" + fifo + "\"";
}

} // namespace

template <> struct ModuleRun<PipeReceiverShape> {
	static constexpr ConverterModule module{.kind = "pipe receiver",
	                                        .word = "pipe-tunnel",
	                                        .body = "PipeReceiverBody",
	                                        .flow = ChannelFlow::nodeToBlock,
	                                        .nodePort = &capturePort};
	using Arguments = ModuleArguments<Home::pipeReceiver>;

	/// The module's arguments, each key from the property table (docs/engine.md#properties-ear-sets says why each): the
	/// one home of this form. Every value is a number, a closed enum, the engine node's name ([A-Za-z0-9_.-]) or the
	/// FIFO ear made the path of, so no user text reaches PipeWire's parsers.
	static Arguments arguments(const CompanionTarget& target, const PipeReceiverShape& shape) {
		const std::size_t channels = target.blockPorts.size();
		const std::string name = "\"" + target.engineNode + "\"";
		ModuleArguments<Home::pipeStream> stream;
		stream.add(property<Home::pipeStream>("media.name"), name)
			.add(property<Home::pipeStream>("node.always-process"), "true")
			.add(property<Home::pipeStream>("adapter.auto-port-config"),
		         "{ mode = dsp monitor = false control = false position = preserve }");
		Arguments arguments;
		arguments.add(property<Home::pipeReceiver>("remote.name"), "\"internal\"")
			.add(property<Home::pipeReceiver>("tunnel.mode"), "\"source\"")
			.add(property<Home::pipeReceiver>("tunnel.may-pause"), "false")
			.add(property<Home::pipeReceiver>("pipe.filename"), quotedFifo(target.fifo))
			.add(property<Home::pipeReceiver>("audio.format"),
		         shape.format == PipeFormat::s16le ? "\"S16LE\"" : "\"S32LE\"")
			.add(property<Home::pipeReceiver>("audio.rate"), std::to_string(shape.rate.hz()))
			.add(property<Home::pipeReceiver>("audio.channels"), std::to_string(channels))
			.add(property<Home::pipeReceiver>("audio.position"), auxPositions(channels))
			.add(property<Home::pipeReceiver>("node.name"), name)
			.add(property<Home::pipeReceiver>("node.description"), name)
			.add(property<Home::pipeReceiver>("stream.props"), stream);
		return arguments;
	}

	static pw_impl_module* load(pw_context* context, const Arguments& arguments) {
		return loadModule(context, "libpipewire-module-pipe-tunnel", arguments);
	}
};

template class ModuleConverters<PipeReceiverShape>;

} // namespace ear::pipewire
