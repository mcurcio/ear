// Claims (docs/writing-a-node-type.md#claims): something ear-wide that one configuration at a time may hold, judged at
// the first readiness step, before anything of the configuration is opened or started. A claim comes from a type's
// explicit CreateOf::claim (a listening TCP port, a UDP port), from each store the spec declares, and from each engine
// companion's shape (claimsOf, companions.hpp), so the framework names no shape's field to claim it.
//
// Who uses what: a type the claim:: ports (CreateOf::claim); NodeHost the rest, its registry being private to ear_core;
// ear_runtime's KernelPortProbe implements PortProbe, which main.cpp hands NodeHost through ResourceAdapters.
// Pure: nothing but the standard library.
#pragma once
#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
namespace ear {
/// One claim: its kind and value. Its key is the two together ("tcp:8928", "udp:5004", "store:sendspin-ma.key",
/// "recording:take1.wav"); a key has one holder ear-wide, across types.
struct Claim {
	/// recording: S3c.5b's file paths, not claimed yet. A sound card's device is not a kind until a card renamed in one
	/// plan is shown to open its device before the old node closed it (docs/design/node-primitives.md 0.2 C).
	enum class Kind { tcp, udp, store, recording };
	Kind kind;
	std::string value;
	friend bool operator==(const Claim&, const Claim&) = default;
};
/// "tcp:8928" | "udp:5004" | "store:<file>" | "recording:<path>": the one home of the keys' spelling, which waitingFor
/// and ear.claim-wait name.
std::string claimKey(const Claim& claim);
/// What a failure about the claim says, `why` after its own words: "cannot listen on port 8928: <why>", "cannot bind UDP
/// port 5004: <why>", "<file>: <why>" (as every store failure), "cannot open <path>: <why>" (as a recording's).
std::string claimText(const Claim& claim, std::string_view why);

namespace claim {
/// A listening TCP port a type's own thread binds (CreateOf::claim): probed with SO_REUSEADDR, so a TIME_WAIT passes and
/// a live listener refuses. Takes a checked whole number (a row's value); outside 1 to 65535 is the type's bug, a
/// std::logic_error.
struct TcpListen {
	explicit TcpListen(std::int64_t port);
	std::uint16_t port;
};
/// A UDP port a type's own thread binds: probed with neither SO_REUSEADDR nor SO_REUSEPORT. Likewise checked.
struct Udp {
	explicit Udp(std::int64_t port);
	std::uint16_t port;
};
Claim of(TcpListen port);
Claim of(Udp port);
} // namespace claim

/// How long a claim waits for a holder that is closing (between its teardown's first and last steps) before the node
/// fails start-failed "... held by node <n>, which did not let go within 15 s". The holder's close deadline (10 s)
/// passes first, so a wait ends in a grant, a port's abandonment, or a refusal naming the leaked configuration.
inline constexpr std::chrono::milliseconds claimWaitBound{15'000};

/// Whether the kernel would let a port's holder bind it now (main thread; a non-blocking socket bound and closed). A
/// program that takes the port just after the probe is not caught. The error is the kernel's reason ("Address already in
/// use"), which follows the claim's words.
class PortProbe {
public:
	/// All IPv4 addresses with SO_REUSEADDR, as a listener binds, so a TIME_WAIT passes and a live listener refuses.
	virtual std::expected<void, std::string> tcpListen(std::uint16_t port) = 0;
	/// All IPv4 addresses with neither SO_REUSEADDR nor SO_REUSEPORT: PipeWire's RTP receiver binds with SO_REUSEADDR
	/// (docs/engine.md#rtp-receivers), so a probe that set it would pass while one holds the port.
	virtual std::expected<void, std::string> udp(std::uint16_t port) = 0;

protected:
	~PortProbe() = default;
};
} // namespace ear
