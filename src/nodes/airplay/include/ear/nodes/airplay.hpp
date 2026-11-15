#pragma once
#include <memory>
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "airplay": an AirPlay 1 speaker (docs/node-types.md#airplay). Its one setting is the row in airplay_spec.hpp. Ports:
/// outputs "out.1" and "out.2", the inside input bank "from_airplay" ("from_airplay.1", "from_airplay.2"), which a pipe
/// receiver fills from the shairport-sync the node runs; the framework passes it straight through to "out". Its listing
/// goes out over mDNS from the node's report loop.
std::unique_ptr<const NodeType> makeAirplayNodeType();
} // namespace ear::nodes
