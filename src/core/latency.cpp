// The latency rules, once (docs/engine.md#latency-per-bank-pair): what lies beyond each bank, what each bank tells,
// and each output's downstream, from the plan by role and what the peers report.
#include "ear/core/latency.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "ear/core/node_type.hpp"

namespace ear::latency_model {
namespace {

/// The one entry for `key` in `entries`, if any. Entries are unique (checked first).
template <class Key> std::optional<Delay> entryOf(const std::vector<std::pair<Key, Delay>>& entries, const Key& key) {
	const auto found = std::ranges::find(entries, key, &std::pair<Key, Delay>::first);
	return found == entries.end() ? std::nullopt : std::optional<Delay>(found->second);
}

template <class Key> bool twice(const std::vector<std::pair<Key, Delay>>& entries) {
	for (auto it = entries.begin(); it != entries.end(); ++it)
		if (std::ranges::find(std::next(it), entries.end(), it->first, &std::pair<Key, Delay>::first) != entries.end())
			return true;
	return false;
}

/// The plan fits the ports: every bank it names is one of theirs (Ports::bank throws otherwise), every pair runs
/// from an input bank to an output bank, every pair delay is on a pair of the plan, and no entry is given twice.
void check(const Ports& ports, const LatencyPlan& plan) {
	for (const BankPair& pair : plan.pairs)
		if (ports.bank(pair.from).direction != Direction::input || ports.bank(pair.to).direction != Direction::output)
			throw std::logic_error("a latency pair runs from an input bank to an output bank: " + pair.from + " to " +
			                       pair.to);
	for (const auto& [pair, delay] : plan.onPair)
		if (std::ranges::find(plan.pairs, pair) == plan.pairs.end())
			throw std::logic_error("a pair's delay names no pair of the plan: " + pair.from + " to " + pair.to);
	for (const auto& [bank, delay] : plan.onBank) static_cast<void>(ports.bank(bank));
	for (const auto& [bank, delay] : plan.beyond) static_cast<void>(ports.bank(bank));
	if (twice(plan.onPair)) throw std::logic_error("a pair's delay is given twice");
	if (twice(plan.onBank)) throw std::logic_error("a bank's delay is given twice");
	if (twice(plan.beyond)) throw std::logic_error("a delay beyond a bank is given twice");
}

} // namespace

LatencyFacts workOut(const Ports& ports, const LatencyPlan& plan, std::span<const std::optional<Latency>> inputPeers,
                     std::span<const std::optional<Latency>> outputPeers) {
	const std::size_t inputCount = ports.inputs().size() + ports.insideInputs().size();
	const std::size_t outputCount = ports.outputs().size() + ports.insideOutputs().size();
	if (inputPeers.size() != inputCount || outputPeers.size() != outputCount)
		throw std::logic_error("latency peers are given one per port: " + std::to_string(inputCount) + " inputs and " +
		                       std::to_string(outputCount) + " outputs");
	check(ports, plan);

	// What lies beyond a bank: its ports' peers side by side (no peer: no path; no path at all: zero, as
	// spa_latency_info_combine_finish makes it), then the companion's delay in series with every one of them.
	const auto beyond = [&](const std::string& name) {
		const Ports::Bank& bank = ports.bank(name);
		const auto peers =
			(bank.direction == Direction::input ? inputPeers : outputPeers).subspan(bank.first, bank.ports.size());
		Paths paths;
		for (const std::optional<Latency>& peer : peers) paths.add(peer);
		const Latency combined = paths.orZero();
		const std::optional<Delay> delay = entryOf(plan.beyond, name);
		return delay ? combined + *delay : combined;
	};
	const auto onPair = [&](const BankPair& pair, Latency path) {
		const std::optional<Delay> delay = entryOf(plan.onPair, pair);
		return delay ? path + *delay : path;
	};

	LatencyFacts facts{std::vector<Latency>(inputCount + outputCount), std::vector<Latency>(outputCount)};
	for (const Ports::Bank& bank : ports.banks()) {
		const bool output = bank.direction == Direction::output;
		// Each pair through the bank is one path, side by side with the others; a bank in no pair tells zero, plus
		// its own delay.
		Paths paths;
		for (const BankPair& pair : plan.pairs) {
			if (output && pair.to == bank.name) paths.add(onPair(pair, beyond(pair.from)));
			if (!output && pair.from == bank.name) paths.add(onPair(pair, beyond(pair.to)));
		}
		const std::optional<Delay> own = entryOf(plan.onBank, bank.name);
		const Latency told = own ? paths.orZero() + *own : paths.orZero();
		const std::size_t base = output ? inputCount : 0;
		std::fill_n(facts.told.begin() + static_cast<std::ptrdiff_t>(base + bank.first),
		            static_cast<std::ptrdiff_t>(bank.ports.size()), told);
		if (output)
			std::fill_n(facts.downstream.begin() + static_cast<std::ptrdiff_t>(bank.first),
			            static_cast<std::ptrdiff_t>(bank.ports.size()), beyond(bank.name));
	}
	return facts;
}

} // namespace ear::latency_model
