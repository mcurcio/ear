// A node's mDNS listing (docs/architecture.md#packages, docs/node-types.md#mdns): one service instance probed, renamed
// on a clash, announced, its questions answered and a goodbye said, over one socket of its own on port 5353, with no
// daemon. The node type keeps the Advertiser in one of its loops' states and turns it there whenever its descriptor is
// readable: it never starts a thread, and probing's waits (RFC 6762's protocol time, not a stand-in for an event) are
// a timerfd of its own behind that descriptor. mjansson/mdns stays inside advertiser.cpp.
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
/// strings. Probed before it is announced, and renamed when another responder holds a name (RFC 6762 sections 8 and
/// 9): `instance` and `host` are the names asked for, Advertiser::listing() the names held.
struct Listing {
	std::string service; // "_raop._tcp", "_sendspin._tcp": "_<name>._tcp" or "_<name>._udp", no ".local"
	// Its instance label: 1 to 63 bytes of UTF-8, no control character and no '.' (mjansson/mdns splits names at dots).
	std::string instance;
	// The label of the host its SRV and A records name: 1 to 63 of [A-Za-z0-9_-] ("_" for sendspin's, which carries
	// the start of a base64url client id and a node name).
	std::string host;
	std::uint16_t port = 0; // 1-65535
	// Each "key=value", 1 to 255 bytes, its key (up to the first "=") 1 or more of printable ASCII. mjansson/mdns writes
	// a TXT string as a key and value pair, so a bare "key" is refused until a listing needs one.
	std::vector<std::string> txt;
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
	/// group through, its address read once here, opens a timerfd (CLOCK_MONOTONIC) and an epoll over both, and starts
	/// probing. Sends nothing yet: the first probe waits a random 0 to 250 ms. Never blocks. Error: listingRefused's words,
	/// or the socket's, the timer's or the epoll's failure in the kernel's words.
	static std::expected<std::unique_ptr<Advertiser>, std::string> open(Listing listing);
	/// The epoll descriptor: readable whenever turn() has work (a datagram waits, or a step of probing or announcing is
	/// due). Its owner calls turn() then (a loop's onReadable), or on its own pace.
	int descriptor() const noexcept;
	/// What one turn changed, for its owner to say (docs/node-types.md#mdns).
	struct Turn {
		/// The listing won its probes and its first announcement went out (RFC 6762 sections 8.1 and 8.3), with the
		/// names it holds now: its instance or its host renamed when a clash made it (section 9). Once per round of
		/// probing: again only after a later clash sent it back to probing.
		std::optional<Listing> announced;
		/// It cannot go on (its socket failed: the kernel's words): it sends nothing more, and its owner drops it.
		std::optional<std::string> failed;
	};
	/// Never blocks. Reads what waits on the socket (at most 32 datagrams), answers questions once announced (from this
	/// link only; each answer once per datagram, by multicast at most once a second per record set, by unicast to a
	/// querier on another port than 5353 at most 16 a second; a probe for a name it holds at once, at most once per 250
	/// ms per record set), hears a clash in a response or a simultaneous probe, and sends what is due: a probe, an
	/// announcement. After failed, every turn returns nothing.
	Turn turn() noexcept;
	/// New TXT strings, the rest unchanged. While probing: used in the later probes and the announcement. Once
	/// announced: announced at once, the cache-flush bit set. Error: as open's, and nothing changes.
	std::expected<void, std::string> update(std::vector<std::string> txt);
	/// The names it probes or holds now (after any rename), and its TXT.
	const Listing& listing() const noexcept;
	/// The goodbye (every record sent again with TTL 0, by multicast) only if it announced, under the names it holds;
	/// then its descriptors closed.
	~Advertiser();
	Advertiser(const Advertiser&) = delete;
	Advertiser& operator=(const Advertiser&) = delete;
	struct State;

private:
	explicit Advertiser(std::unique_ptr<State> state);
	std::unique_ptr<State> state_;
};

} // namespace ear::mdns
