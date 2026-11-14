// The claim registry (docs/writing-a-node-type.md#claims; ear/core/claims.hpp): who holds each claim ear-wide, judged
// at a configuration's first readiness step. Private to ear_core: NodeHost keeps one, on the main loop. It probes
// nothing: a port it grants is NodeHost's to probe (behind the turn's flush, so ear's own receiver that a restart just
// destroyed has let go of it first).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ear/core/claims.hpp"
#include "ear/core/graph_spec.hpp"

namespace ear::detail {

/// One configuration as a claim's holder or claimant: its generation (unique across nodes), its node's serial and name.
struct ClaimHolder {
	std::uint64_t generation = 0;
	std::uint64_t serial = 0;
	NodeName node;
};

/// How a claim was judged.
struct ClaimJudged {
	/// granted: held from now on. probe: held from now on, once the kernel agrees (a port: NodeHost probes it, and a
	/// refusal fails the claimant). refused: never, `why` after the claim's words. waits: its holder is closing, `why` its
	/// node; judged again when that one lets go or is leaked.
	enum class Outcome { granted, probe, refused, waits };
	Outcome outcome = Outcome::granted;
	std::string why{};
	/// The generation of a leaked configuration that refused it (a store or recording it keeps) or whose abandoned port
	/// it probes, else 0: a refusal on its account is that configuration's to undo when it closes late.
	std::uint64_t leaked = 0;
};

/// A waiter's claim judged once its holder let go or was leaked.
struct ClaimDecided {
	ClaimHolder waiter;
	Claim claim;
	ClaimJudged judged;
};

/// Main thread only. A holder is live (configured or readying), going (its teardown began: a claimant waits), or leaked
/// (past its close deadline: its ports are abandoned to whoever probes them next, its stores and recordings kept).
class ClaimRegistry {
public:
	/// `claimant` asks for `claim`: granted (or probe, for a port) with no holder, or held by its own configuration;
	/// refused "held by node <n>" while a live configuration of another holds it; waits while the holder is going (in
	/// arrival order); held by a leaked configuration, a port is probed afresh (its holder the claimant from now on), a
	/// store or recording refused "held by node <n>'s earlier configuration, which did not close; it starts when that
	/// closes".
	ClaimJudged claim(const ClaimHolder& claimant, const Claim& claim);
	/// `generation`'s teardown began: its claims are going, and its own waits leave their queues.
	void going(std::uint64_t generation);
	/// `generation` was leaked: its ports abandoned, its stores and recordings kept. What waited for them is judged now.
	std::vector<ClaimDecided> leaked(std::uint64_t generation);
	/// `generation` let go (teardown step 5, or a leaked configuration's late close): every claim it held released and
	/// its waits dropped. What waited for those is judged again, in arrival order.
	std::vector<ClaimDecided> release(std::uint64_t generation);
	/// Whether `generation` holds any claim.
	bool holds(std::uint64_t generation) const noexcept;

private:
	enum class State { live, going, leaked };
	struct Held {
		Claim claim;
		ClaimHolder holder;
		State state = State::live;
	};
	struct Waiting {
		std::string key;
		Claim claim;
		ClaimHolder waiter;
	};
	/// Judges every wait on `keys` again, in arrival order.
	std::vector<ClaimDecided> rejudge(const std::vector<std::string>& keys);
	void dropWaits(std::uint64_t generation);

	std::map<std::string, Held> held_;
	std::vector<Waiting> waiting_; // in arrival order
};

} // namespace ear::detail
