// The framework's pass-through (docs/writing-a-node-type.md#the-class): what `card` and `rtp` run, so no
// type writes audio code to join an outside node to its public ports. NodeHost makes it from a NodeCode::passThrough;
// a type's own tests make it the same way to check what their pairs carry.
#pragma once
#include <memory>
#include <vector>
#include "ear/core/node_type.hpp"
namespace ear {
/// An AudioCode copying each pair's `from` bank to its `to` bank, port by port, and silencing every output no pair
/// names. Throws std::logic_error (a type bug) when a pair names a bank `ports` lacks, a `from` that is not an input, a
/// `to` that is not an output, banks of different sizes, an output bank twice, or no pair at all.
std::unique_ptr<AudioCode> makePassThrough(const Ports& ports, const std::vector<BankPair>& pairs);
} // namespace ear
