// How a node type's ports are declared: banks, routable and inside, and the one place a port's whole name is made
// (node_type.hpp).
#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include "ear/core/node_type.hpp"

namespace ear {
namespace {

/// A bank's name or a member: 1 or more of [A-Za-z0-9_-], so the whole name has one '.' and no ':'.
bool validPart(std::string_view part) {
	return !part.empty() && std::all_of(part.begin(), part.end(), [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
	});
}

} // namespace

PortName portName(std::string_view bank, std::string_view member) {
	if (!validPart(bank) || !validPart(member))
		throw std::logic_error("a port bank's name and members are letters, digits, - and _: bank \"" +
		                       std::string(bank) + "\", member \"" + std::string(member) + "\"");
	return std::string(bank) + '.' + std::string(member);
}

std::vector<std::string> numbered(std::uint32_t count) {
	std::vector<std::string> names;
	names.reserve(count);
	for (std::uint32_t i = 1; i <= count; ++i) names.push_back(std::to_string(i));
	return names;
}

Ports::Ports(const std::vector<PortBank>& banks) {
	for (const PortBank& declared : banks) {
		if (std::ranges::any_of(banks_, [&](const Bank& known) { return known.name == declared.name; }))
			throw std::logic_error("a node's port banks have unique names: \"" + std::string(declared.name) +
			                       "\" is declared twice");
		const bool inside = declared.reach == PortBank::Reach::inside;
		auto& list = declared.direction == Direction::input ? (inside ? insideInputs_ : inputs_)
		                                                    : (inside ? insideOutputs_ : outputs_);
		Bank bank{std::string(declared.name), declared.direction, declared.reach, {}, list.size()};
		for (const std::string& member : declared.members) {
			bank.ports.push_back(portName(declared.name, member));
			list.push_back(bank.ports.back());
		}
		banks_.push_back(std::move(bank));
	}
	// An inside bank's ports follow every routable port of its direction in the spans process() gets.
	for (Bank& bank : banks_)
		if (bank.reach == PortBank::Reach::inside)
			bank.first += bank.direction == Direction::input ? inputs_.size() : outputs_.size();
}

const Ports::Bank& Ports::bank(std::string_view name) const {
	const auto found = std::ranges::find_if(banks_, [&](const Bank& bank) { return bank.name == name; });
	if (found == banks_.end()) throw std::logic_error("no port bank \"" + std::string(name) + "\"");
	return *found;
}

Ports Ports::publicOnly() const {
	Ports only;
	only.inputs_ = inputs_;
	only.outputs_ = outputs_;
	for (const Bank& bank : banks_)
		if (bank.reach == PortBank::Reach::routable) only.banks_.push_back(bank);
	return only;
}

} // namespace ear
