// The name a listening sendspin node goes by over mDNS (docs/node-types.md#mdns): its listing is ear_mdns's, under
// this instance name, which is its host label too.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>
namespace ear::nodes::sendspin {
/// `ear-<first 50 bytes of node>-<first 8 of client id>`: within a DNS label's 63 bytes, stable and unique without
/// probing.
inline std::string advertisedInstance(std::string_view node, std::string_view clientId) {
	constexpr std::size_t maxNodeBytes = 50;
	constexpr std::size_t maxIdBytes = 8;
	return "ear-" + std::string(node.substr(0, maxNodeBytes)) + "-" + std::string(clientId.substr(0, maxIdBytes));
}
} // namespace ear::nodes::sendspin
