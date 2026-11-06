// mDNS for a listening sendspin node (docs/design/sendspin.md section 4): `_sendspin._tcp` on its port, so a server finds
// it. mjansson/mdns stays inside advertiser.cpp. One socket per node, on the worker thread only.
#pragma once
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
namespace ear::sendspin {
/// `ear-<first 50 bytes of node>-<first 8 of client id>`: within a DNS label's 63 bytes, stable and unique without
/// probing.
std::string advertisedInstance(std::string_view node, std::string_view clientId);
/// Whether a query's source shares this host's link, which is all an mDNS responder may answer (RFC 6762 section 11): on the
/// subnet of `localAddress`, or this host itself. All three in network byte order.
bool onLink(std::uint32_t sourceAddress, std::uint32_t localAddress, std::uint32_t netmask) noexcept;
class Advertiser {
public:
	/// Announces PTR, SRV, TXT (path=/sendspin, name=<name>) and A. Error: its detail.
	static std::expected<std::unique_ptr<Advertiser>, std::string> open(std::string instance, std::string name,
	                                                                    std::uint16_t port);
	/// Never blocks. Queries from this link only; each answer once per datagram, by multicast once a second, by unicast within
	/// a budget; whatever the datagram asks.
	void answer() noexcept;
	~Advertiser(); // goodbye
	struct State;

private:
	explicit Advertiser(std::unique_ptr<State> state);
	std::unique_ptr<State> state_;
};
} // namespace ear::sendspin
