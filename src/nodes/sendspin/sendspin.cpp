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
	const std::uint32_t feedFrames = feedFramesFor(format);
	// The library's own thread is never held back long by a graph that does not drain the feed (a stop, a long stall):
	// past undrainedBound the feed drops what it is given at the graph's rate (docs/node-types.md#clock).
	bell_ = std::make_unique<EventFd>();
	feed_ = std::make_unique<Feed>(nodeChannels, feedFrames, FeedDrain{.rate = format.rate, .bell = bell_.get()});

	cx.loop(worker, std::make_unique<Worker>(WorkerParts{.handle = node_.handle(),
	                                                     .keys = cx.store(keyFile),
	                                                     .feed = *feed_,
	                                                     .player = player_,
	                                                     .self = cx.executor(worker),
	                                                     .settings = std::move(settings),
	                                                     .format = format}));
	cx.loop(notifier, std::make_unique<Notifier>(cx.executor(notifier), *bell_, progress_, *feed_, player_));
	// Staged until the node is configured: so the key store is open and trusted, and the sink's consumer began, before
	// the worker's first tick makes the client.
	cx.post(worker, [](Worker& state) { state.begin(); });
	cx.post(notifier, [](Notifier& state) { state.begin(); });

	// The sink to the server holds half a second.
	const auto half = static_cast<std::uint32_t>(FrameClock(format.rate).frames(std::chrono::milliseconds{500}));
	cx.sink(toServer, SinkShape{.frames = half}).consumedBy(worker, &Worker::toServer);
	// The feed holds the server's audio up to its size before it leaves (docs/engine.md#latency-per-bank-pair); `to`
	// reaches the sink with nothing added, its path carried so a chunk knows what lies upstream of it.
	cx.declareLatency(fromServer, from, Latency{0, feedFrames});
	cx.declareLatency(to, toServer, Latency{});
	cx.use(SendspinNodeCode{std::make_unique<SendspinCode>(*feed_, progress_, *bell_, format.rate)});
	return Configured::applied();
}

} // namespace ear::nodes::sendspin

namespace ear::nodes {

std::unique_ptr<const NodeType> makeSendspinNodeType() { return makeNodeType<sendspin::spec, sendspin::Sendspin>(); }

} // namespace ear::nodes
