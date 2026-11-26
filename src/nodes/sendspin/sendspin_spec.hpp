// The "sendspin" node type's class on its spec (spec.hpp; docs/node-types.md#sendspin,
// docs/writing-a-node-type.md#the-class): private to the type, shared by sendspin.cpp and its tests, which run it through ear::testing::TypeHarness<spec,
// Sendspin> by the same handles. In the type's own named namespace, never an unnamed one (type_spec.hpp says why).
//
// Its ports are fixed; the connection is the worker's, so an absent server is no failure: the node runs and its outputs
// are silent. Every setting changes in place: the worker swaps its client (docs/node-types.md#changes-in-place). The library's client lives on the worker loop for the whole configuration: a stop silences the node's
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
#include "key_store.hpp"
#include "sendspin_clock.hpp"
#include "settings.hpp"
#include "spec.hpp"
#include "worker.hpp"

namespace ear::nodes::sendspin {

using Checked = CheckedOf<spec>;

/// The source's ring: small, so a correction lands within a chunk: the next power of two at or above 40 ms and two
/// blocks.
inline std::uint32_t sourceFramesFor(const AudioFormat& format) {
	const auto forty = static_cast<std::uint32_t>(FrameClock(format.rate).frames(std::chrono::milliseconds{40}));
	return std::bit_ceil(std::max(forty, 2 * format.maxBlock));
}

/// One configuration's instance. It owns the player slot both loops' states share, freed at teardown step 5 after
/// the states; the clock is the source's, freed with it after the instance. Main loop only; it reaches the states only
/// through the framework.
class Sendspin {
public:
	static Members<spec> ports(const Checked&) {
		return {to(members("left", "right")), from(members("left", "right")), toServer(members("left", "right")),
		        fromServer(members("left", "right"))};
	}
	explicit Sendspin(NodeOf<spec>& node) : node_(node) {}
	/// A listening node claims its port (judged before anything opens); the key store opens before the loops run.
	/// Makes the source on `from_server` with its clock, the worker and the notifier states (nothing armed), the sink
	/// on `to_server` consumed by the worker's ToServer, and the pass-through; the loops start the client and the watch
	/// once the node is configured.
	Configured configure(CreateOf<spec>& cx, const Checked& s);
	/// A change of `name`, `port` or `server`, the switch between listening and dialling included
	/// (docs/node-types.md#changes-in-place): the old listening port released at the commit point and a new one claimed
	/// now, then the worker swaps the client for one of the new settings over the same key store, the sink paused.
	Changed configure(ChangeOf<spec>& cx, const Checked& old, const Checked& now);

private:
	NodeOf<spec>& node_;
	PlayerSlot player_;
};

} // namespace ear::nodes::sendspin
