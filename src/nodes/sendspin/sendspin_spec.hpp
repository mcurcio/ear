// The "sendspin" node type's spec and class (docs/node-types.md#sendspin; docs/writing-a-node-type.md#the-class):
// private to the type, shared by sendspin.cpp and its tests, which run it through ear::testing::TypeHarness<spec,
// Sendspin> by the same handles. In the type's own named namespace, never an unnamed one (type_spec.hpp says why).
//
// Its ports are fixed; the connection is the worker's, so an absent server is no failure: the node runs and its outputs
// are silent. The library's client lives on the worker loop for the whole configuration: a stop silences the node's
// audio both ways (the block's silence) and keeps the connection and pairing, so the server still sees the player and a
// start plays again at once; a restart, a disable or a removal tears the configuration down, and the worker's state,
// destroyed, says goodbye (docs/node-types.md#stop-restart-disable).
#pragma once
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>

#include "ear/core/lifecycle.hpp"
#include "event_fd.hpp"
#include "feed.hpp"
#include "key_store.hpp"
#include "mailbox.hpp"
#include "sendspin_code.hpp"
#include "settings.hpp"
#include "worker.hpp"

namespace ear::nodes::sendspin {

inline constexpr auto to = bank::input<"to">;
inline constexpr auto from = bank::output<"from">;
/// The sink's bank: `to` passed straight through, read by ToServer on the sink's thread; never faded.
inline constexpr auto toServer = bank::insideOutput<"to_server">;
/// What the server sends, before `from`. Nothing joins it yet: the feed plays into `from` in the node's code, and its
/// latency is declared on the path from here to `from` (docs/writing-a-node-type.md#latency).
inline constexpr auto fromServer = bank::insideInput<"from_server">;
/// `sendspin-<node>.key` in the graph file folder: mode 0600, at most 64 KiB, created as an empty key record, trusted
/// only as one (docs/node-types.md#the-identity-file).
inline constexpr auto keyFile = store::file<"key">(
	{.mode = 0600, .maxBytes = maxKeyFileBytes, .initial = &emptyKeyRecord, .distrust = &whyNotKeyRecord});
/// The library's main loop: its client, pairing, the time filter, mDNS (`ear-sendspin <node>`).
inline constexpr auto worker = loop::dedicated<"sendspin", Worker>;
/// What played, told to the library apart from the worker (`ear-notify <node>`).
inline constexpr auto notifier = loop::dedicated<"notify", Notifier>;
inline constexpr TypeSpec spec{.settings = settingsTable,
                               .banks = Banks<to, from, toServer, fromServer>{},
                               .stores = Stores<keyFile>{},
                               .loops = Loops<worker, notifier>{}};
using Checked = CheckedOf<spec>;

/// Small, so a correction lands within a chunk: the next power of two at or above 40 ms and two blocks.
inline std::uint32_t feedFramesFor(const AudioFormat& format) {
	const auto forty = static_cast<std::uint32_t>(FrameClock(format.rate).frames(std::chrono::milliseconds{40}));
	return std::bit_ceil(std::max(forty, 2 * format.maxBlock));
}

/// The node's code: `to` passed to the sink's bank, and the feed played into `from` (sendspin_code.hpp).
struct SendspinNodeCode {
	std::unique_ptr<SendspinCode> playback;
	void process(Io<spec> io, const GraphCycle& cycle) noexcept {
		const auto in = io[to];
		const auto out = io[toServer];
		for (std::size_t channel = 0; channel < in.size(); ++channel)
			std::ranges::copy(in[channel], out[channel].begin());
		playback->process(io[from], io.frames(), cycle);
	}
};

/// One configuration's instance. It owns what the code and both loops' states share (the feed, the bell and the
/// mailbox the code tells what played through, the player slot), freed at teardown step 5 after the code and the
/// states. Main loop only; it reaches the states only through the framework.
class Sendspin {
public:
	static Members<spec> ports(const Checked&) {
		return {to(members("left", "right")), from(members("left", "right")), toServer(members("left", "right")),
		        fromServer(members("left", "right"))};
	}
	explicit Sendspin(NodeOf<spec>& node) : node_(node) {}
	/// A listening node claims its port (judged before anything opens); the key store opens before the loops run.
	/// Makes the feed, the worker and the notifier states (nothing armed), the sink on `to_server` consumed by the
	/// worker's ToServer, and the code; the loops start the client and the watch once the node is configured.
	Configured configure(CreateOf<spec>& cx, const Checked& s);

private:
	NodeOf<spec>& node_;
	std::unique_ptr<Feed> feed_;
	std::unique_ptr<EventFd> bell_;
	Mailbox<PlayProgress> progress_;
	PlayerSlot player_;
};

} // namespace ear::nodes::sendspin
