// The "sendspin" node type: Music Assistant's audio in and out over the Sendspin protocol (docs/node-types.md#sendspin,
// ear/nodes/sendspin.hpp). Its spec and class are sendspin_spec.hpp's.
#include "ear/nodes/sendspin.hpp"

#include <chrono>
#include <cstdint>
#include <memory>

#include "sendspin_spec.hpp"

namespace ear::nodes::sendspin {

Configured Sendspin::configure(CreateOf<spec>& cx, const Checked& s) {
	SendspinSettings settings = settingsFrom(cx.node(), s);
	// The library only logs a port it cannot bind and retries, so a listening node's port is judged before anything of
	// it opens: across ear's nodes, then by the kernel (docs/writing-a-node-type.md#claims).
	if (!settings.server) cx.claim(claim::TcpListen(settings.port));
	const AudioFormat format = cx.format();
	const std::uint32_t sourceFrames = sourceFramesFor(format);
	// The library's audio reaches `from` through a source whose clock is the node's: it skips the graph's lost time,
	// tells the library what played, and, past its bound with no tick draining the source (a stop, a long stall), plays
	// what the library writes into nothing at the graph's rate (docs/node-types.md#clock). The source owns the clock and
	// frees it after the instance and the states, which keep references to it.
	auto owned = std::make_unique<SendspinClock>(
		SendspinClock::Config{.rate = format.rate, .undrainedBound = undrainedBoundFor(format.rate, sourceFrames)});
	SendspinClock& clock = *owned;
	SourceWriter& writer = cx.source(fromServer, SourceShape{.frames = sourceFrames}, std::move(owned));
	clock.attach(writer);

	cx.loop(worker, std::make_unique<Worker>(WorkerParts{.handle = node_.handle(),
	                                                     .keys = cx.store(keyFile),
	                                                     .clock = clock,
	                                                     .writer = writer,
	                                                     .player = player_,
	                                                     .self = cx.executor(worker),
	                                                     .settings = std::move(settings),
	                                                     .format = format}));
	cx.loop(notifier, std::make_unique<Notifier>(cx.executor(notifier), clock, player_));
	// Staged until the node is configured: so the key store is open and trusted, and the sink's consumer began, before
	// the worker's first tick makes the client.
	cx.post(worker, [](Worker& state) { state.begin(); });
	cx.post(notifier, [](Notifier& state) { state.begin(); });

	// The sink to the server holds half a second.
	const auto half = static_cast<std::uint32_t>(FrameClock(format.rate).frames(std::chrono::milliseconds{500}));
	cx.sink(toServer, SinkShape{.frames = half}, worker, &Worker::toServer);
	// The source declares its ring as latency beyond its bank (docs/writing-a-node-type.md#sources), which `from`
	// tells through the pair; `to` reaches the sink with nothing added, its path carried so a chunk knows what lies
	// upstream of it.
	cx.use(passThrough({bankPair(fromServer, from), bankPair(to, toServer)}));
	return Configured::applied();
}

} // namespace ear::nodes::sendspin

namespace ear::nodes {

std::unique_ptr<const NodeType> makeSendspinNodeType() { return makeNodeType<sendspin::spec, sendspin::Sendspin>(); }

} // namespace ear::nodes
