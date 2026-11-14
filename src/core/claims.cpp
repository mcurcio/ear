// Claims' keys, texts and ports (ear/core/claims.hpp), and the registry that judges them (claims.hpp;
// docs/writing-a-node-type.md#claims).
#include "claims.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ear {
namespace {

std::uint16_t checkedPort(std::int64_t port, const char* what) {
	if (port < 1 || port > 65535)
		throw std::logic_error(std::string(what) + " outside 1-65535: " + std::to_string(port));
	return static_cast<std::uint16_t>(port);
}

bool isPort(Claim::Kind kind) noexcept { return kind == Claim::Kind::tcp || kind == Claim::Kind::udp; }

} // namespace

std::string claimKey(const Claim& claim) {
	switch (claim.kind) {
	case Claim::Kind::tcp:
		return "tcp:" + claim.value;
	case Claim::Kind::udp:
		return "udp:" + claim.value;
	case Claim::Kind::store:
		return "store:" + claim.value;
	case Claim::Kind::recording:
		return "recording:" + claim.value;
	}
	return claim.value;
}

std::string claimText(const Claim& claim, std::string_view why) {
	switch (claim.kind) {
	case Claim::Kind::tcp:
		return "cannot listen on port " + claim.value + ": " + std::string(why);
	case Claim::Kind::udp:
		return "cannot bind UDP port " + claim.value + ": " + std::string(why);
	case Claim::Kind::store:
		return claim.value + ": " + std::string(why);
	case Claim::Kind::recording:
		return "cannot open " + claim.value + ": " + std::string(why);
	}
	return claim.value + ": " + std::string(why);
}

namespace claim {
TcpListen::TcpListen(std::int64_t port) : port(checkedPort(port, "a TCP port")) {}
Udp::Udp(std::int64_t port) : port(checkedPort(port, "a UDP port")) {}
Claim of(TcpListen port) { return Claim{Claim::Kind::tcp, std::to_string(port.port)}; }
Claim of(Udp port) { return Claim{Claim::Kind::udp, std::to_string(port.port)}; }
} // namespace claim

namespace detail {

ClaimJudged ClaimRegistry::claim(const ClaimHolder& claimant, const Claim& claim) {
	const std::string key = claimKey(claim);
	const auto free = [&] {
		return ClaimJudged{isPort(claim.kind) ? ClaimJudged::Outcome::probe : ClaimJudged::Outcome::granted};
	};
	const auto found = held_.find(key);
	if (found == held_.end()) {
		held_.emplace(key, Held{claim, claimant, State::live});
		return free();
	}
	Held& held = found->second;
	if (held.holder.generation == claimant.generation) return ClaimJudged{ClaimJudged::Outcome::granted};
	switch (held.state) {
	case State::live:
		return ClaimJudged{ClaimJudged::Outcome::refused, "held by node " + held.holder.node};
	case State::going:
		waiting_.push_back(Waiting{key, claim, claimant});
		return ClaimJudged{ClaimJudged::Outcome::waits, held.holder.node};
	case State::leaked: {
		const std::uint64_t leaked = held.holder.generation;
		if (isPort(claim.kind)) { // abandoned: whoever probes it next holds it
			held = Held{claim, claimant, State::live};
			return ClaimJudged{ClaimJudged::Outcome::probe, {}, leaked};
		}
		return ClaimJudged{ClaimJudged::Outcome::refused,
		                   "held by node " + held.holder.node +
		                       "'s earlier configuration, which did not close; it starts when that closes",
		                   leaked};
	}
	}
	return free();
}

void ClaimRegistry::going(std::uint64_t generation) {
	for (auto& [key, held] : held_)
		if (held.holder.generation == generation && held.state == State::live) held.state = State::going;
	dropWaits(generation);
}

std::vector<ClaimDecided> ClaimRegistry::leaked(std::uint64_t generation) {
	std::vector<std::string> keys;
	for (auto& [key, held] : held_)
		if (held.holder.generation == generation) {
			held.state = State::leaked;
			keys.push_back(key);
		}
	dropWaits(generation);
	return rejudge(keys);
}

std::vector<ClaimDecided> ClaimRegistry::release(std::uint64_t generation) {
	std::vector<std::string> keys;
	for (auto it = held_.begin(); it != held_.end();) {
		if (it->second.holder.generation != generation) {
			++it;
			continue;
		}
		keys.push_back(it->first);
		it = held_.erase(it);
	}
	dropWaits(generation);
	return rejudge(keys);
}

bool ClaimRegistry::holds(std::uint64_t generation) const noexcept {
	return std::ranges::any_of(held_, [&](const auto& entry) { return entry.second.holder.generation == generation; });
}

std::vector<ClaimDecided> ClaimRegistry::rejudge(const std::vector<std::string>& keys) {
	std::vector<ClaimDecided> decided;
	if (keys.empty()) return decided;
	std::vector<Waiting> again;
	std::vector<Waiting> kept;
	for (Waiting& waiting : waiting_)
		(std::ranges::find(keys, waiting.key) != keys.end() ? again : kept).push_back(std::move(waiting));
	waiting_ = std::move(kept);
	for (Waiting& waiting : again) {
		ClaimJudged judged = claim(waiting.waiter, waiting.claim); // a wait again re-enters the queue
		decided.push_back(ClaimDecided{std::move(waiting.waiter), std::move(waiting.claim), std::move(judged)});
	}
	return decided;
}

void ClaimRegistry::dropWaits(std::uint64_t generation) {
	std::erase_if(waiting_, [&](const Waiting& waiting) { return waiting.waiter.generation == generation; });
}

} // namespace detail
} // namespace ear
