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
	SourceWriter& writer = cx.attach(fromServer, SourceShape{.frames = sourceFrames}, std::move(owned));
	clock.attach(writer);

	// The sink to the server holds half a second. Its control is the worker's, which pauses it while it swaps the
	// client a change in place makes (docs/writing-a-node-type.md#pausing-a-sink).
	const auto half = static_cast<std::uint32_t>(FrameClock(format.rate).frames(std::chrono::milliseconds{500}));
	SinkControl& toServerSink = cx.attach(toServer, SinkShape{.frames = half}, worker, &Worker::toServer);

	auto working = cx.state(worker, WorkerParts{.handle = node_.handle(),
	                                            .keys = cx.store(keyFile),
	                                            .clock = clock,
	                                            .writer = writer,
	                                            .player = player_,
	                                            .self = cx.executor(worker),
	                                            .sink = toServerSink,
	                                            .settings = std::move(settings),
	                                            .format = format});
	auto notifying = cx.state(notifier, cx.executor(notifier), clock, player_);
	// Staged until the node is configured: so the key store is open and trusted, and the sink's consumer began, before
	// the worker's first tick makes the client.
	cx.post(worker, [](Worker& state) { state.begin(); });
	cx.post(notifier, [](Notifier& state) { state.begin(); });

	// The source declares its ring as latency beyond its bank (docs/writing-a-node-type.md#sources), which `from`
	// tells through the pair; `to` reaches the sink with nothing added, its path carried so a chunk knows what lies
	// upstream of it.
	return cx.applied(passThrough(bankPair(fromServer, from), bankPair(to, toServer)), std::move(working),
	                  std::move(notifying));
}

Changed Sendspin::configure(ChangeOf<spec>& cx, const Checked& old, const Checked& now) {
	const SendspinSettings was = settingsFrom(cx.node(), old);
	SendspinSettings next = settingsFrom(cx.node(), now);
	// The library fixes its configuration when a client is made, so every change is a new client; a listening port is
	// held by both for a moment, the old one released at the commit point, and a new one judged as the change version
	// returns, as at a start (docs/writing-a-node-type.md#claims-in-a-change). The same port claimed and unclaimed stays
	// the node's.
	if (!was.server) cx.unclaim(claim::TcpListen(was.port));
	if (!next.server) cx.claim(claim::TcpListen(next.port));
	return cx.whenReady(worker, [next = std::move(next)](Worker& state, ConfigureDone done) mutable {
		state.swapClient(std::move(next), std::move(done));
	});
}

} // namespace ear::nodes::sendspin

namespace ear::nodes {

std::unique_ptr<const NodeType> makeSendspinNodeType() { return makeNodeType<sendspin::spec, sendspin::Sendspin>(); }

} // namespace ear::nodes
