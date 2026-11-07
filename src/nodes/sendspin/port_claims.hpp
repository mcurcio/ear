// Which node holds which listening port (docs/design/sendspin.md section 10). The library only logs a port it cannot bind
// and retries, so a node told it can listen must be checked before its worker starts: across ear's nodes here, then a
// probe of the kernel. A program taking the port just after the probe is not caught.
#pragma once
#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include "ear/core/graph_spec.hpp"
namespace ear::sendspin {
/// Main thread only; held by the type; a claim is released when the node's insides go.
class PortClaims {
public:
	/// Error: why not (the detail after `cannot listen on port <p>: `).
	std::expected<void, std::string> claim(std::uint16_t port, const NodeName& node);
	void release(std::uint16_t port);

private:
	std::map<std::uint16_t, NodeName> held_;
};
/// Whether the library's listener could bind `port`: all addresses with SO_REUSEADDR, as it does, so a live listener
/// refuses and a TIME_WAIT from the node's last run does not. Error: the kernel's reason.
std::expected<void, std::string> probeListenPort(std::uint16_t port);
} // namespace ear::sendspin
