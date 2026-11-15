// A node's mDNS listing (docs/architecture.md#packages, docs/node-types.md#mdns): one service instance announced, its
// questions answered and a goodbye said, over one socket of its own on port 5353, with no daemon. The node type keeps
// the Advertiser in one of its loops' states and runs it there: it never starts a thread or arms a timer, so whatever
// loop it lives on paces it (sendspin's worker turns it every 10 ms; airplay's report loop answers when its descriptor
// is readable). mjansson/mdns stays inside advertiser.cpp.
#pragma once
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace ear::mdns {

/// One DNS-SD service instance (RFC 6763): `<instance>.<service>.local.` on `<host>.local.`, port `port`, with its TXT
/// strings. Announced without probing: the caller makes `instance` and `host` unique (sendspin by its client id).
struct Listing {
	std::string service;    // "_raop._tcp", "_sendspin._tcp": "_<name>._tcp" or "_<name>._udp", no ".local"
	std::string instance;   // its instance label: 1 to 63 bytes of UTF-8, no control character
	std::string host;       // the label of the host its SRV and A records name: 1 to 63 of [A-Za-z0-9-]
	std::uint16_t port = 0; // 1-65535
	std::vector<std::string>
		txt; // each "key=value" or "key", 1 to 255 bytes; its key 1 or more of printable ASCII but "="
	friend bool operator==(const Listing&, const Listing&) = default;
};

/// Why `listing` cannot be announced (the first rule above it breaks, in words), else nothing: the one check, which
/// Advertiser::open and update apply and a caller may apply first.
std::optional<std::string> listingRefused(const Listing& listing);

/// Whether a query's source shares this host's link, which is all an mDNS responder may answer (RFC 6762 section 11):
/// on the subnet of `localAddress`, or this host itself. All three in network byte order.
bool onLink(std::uint32_t sourceAddress, std::uint32_t localAddress, std::uint32_t netmask) noexcept;

/// One listing on the network, from open until destroyed. Its thread is whoever calls it; one thread at a time.
class Advertiser {
public:
	/// Joins 224.0.0.251 on port 5353 (shared: SO_REUSEADDR and SO_REUSEPORT) on the one interface the kernel routes the
	/// group through, its address read once here, and announces PTR, SRV, TXT and A by multicast. Never blocks. Error: a
	/// listing listingRefused refuses, or the socket's failure, in words.
	static std::expected<std::unique_ptr<Advertiser>, std::string> open(Listing listing);
	/// Readable whenever a datagram waits: its owner calls answer() then (a loop's onReadable), or on its own pace.
	int descriptor() const noexcept;
	/// Never blocks. Queries from this link only, at most 32 datagrams a call; each answer once per datagram, by
	/// multicast at most once a second per record set, by unicast (to a querier on another port than 5353) at most 16
	/// a second; responses and anything else ignored.
	void answer() noexcept;
	/// New TXT strings, the rest unchanged: announced at once, the cache-flush bit set. Error: as open's, and nothing
	/// changes.
	std::expected<void, std::string> update(std::vector<std::string> txt);
	const Listing& listing() const noexcept;
	/// The goodbye: every record sent again with TTL 0, by multicast; then the socket closed.
	~Advertiser();
	Advertiser(const Advertiser&) = delete;
	Advertiser& operator=(const Advertiser&) = delete;
	struct State;

private:
	explicit Advertiser(std::unique_ptr<State> state);
	std::unique_ptr<State> state_;
};

} // namespace ear::mdns
