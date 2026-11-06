// How a node type's ports are declared: banks and the one place a public port's whole name is made
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

std::vector<std::string> numbered(std::uint32_t count, std::string_view prefix) {
	std::vector<std::string> names;
	names.reserve(count);
	for (std::uint32_t i = 1; i <= count; ++i) names.push_back(std::string(prefix) + std::to_string(i));
	return names;
}

Ports::Ports(const std::vector<PortBank>& banks, std::vector<PortName> insideInputs,
             std::vector<PortName> insideOutputs)
	: insideInputs_(std::move(insideInputs)), insideOutputs_(std::move(insideOutputs)) {
	for (const PortBank& bank : banks) {
		auto& list = bank.direction == Direction::input ? inputs_ : outputs_;
		for (const std::string& member : bank.members) list.push_back(portName(bank.name, member));
	}
}

Ports Ports::publicOnly() const {
	Ports only;
	only.inputs_ = inputs_;
	only.outputs_ = outputs_;
	return only;
}

} // namespace ear
