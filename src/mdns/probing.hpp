// The pure parts of probing and renaming a listing (RFC 6762 sections 8 and 9; docs/node-types.md#mdns): the names a
// clash gives, the tiebreak of two simultaneous probes, and the pace probing keeps after many clashes. Private to
// ear_mdns: advertiser.cpp drives them over its socket, and ear_mdns_tests test them with no socket.
#pragma once
#include <chrono>
#include <compare>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>
namespace ear::mdns::probing {

/// Probing's steps (section 8.1): a random wait in [0, firstProbeMost] before the first probe, probeCount probes
/// probeInterval apart, then probeInterval more before the first announcement.
inline constexpr std::chrono::milliseconds firstProbeMost{250};
inline constexpr std::chrono::milliseconds probeInterval{250};
inline constexpr int probeCount = 3;
/// The loser of a simultaneous probe waits this long, then probes the same names again (section 8.2).
inline constexpr std::chrono::milliseconds lostTiebreakWait{1'000};
/// The second announcement follows the first by this (section 8.3).
inline constexpr std::chrono::milliseconds announceInterval{1'000};
/// An answer to a probe goes out by multicast at most this often per record set, inside the one-second limit every
/// other multicast answer keeps (section 6).
inline constexpr std::chrono::milliseconds probeAnswerInterval{250};

/// `asked`'s `n`th name (n at least 2) after clashes: "<asked> (<n>)", `asked` cut at a UTF-8 character boundary so
/// the whole is at most 63 bytes (a DNS label's most).
std::string instanceNamed(std::string_view asked, unsigned n);
/// `asked`'s `n`th name (n at least 2) after clashes: "<asked>-<n>", `asked` cut so the whole is at most 63 bytes.
std::string hostNamed(std::string_view asked, unsigned n);

/// One record as section 8.2 compares two: its class with the cache-flush bit cleared, its type, then its rdata as
/// uncompressed bytes, each an unsigned value.
struct RecordKey {
	std::uint16_t rrclass = 1; // IN
	std::uint16_t type = 0;
	std::string rdata;
	friend auto operator<=>(const RecordKey&, const RecordKey&) = default;
};
/// An SRV record's key as the advertiser sends it: priority 0, weight 0, `port`, then `target` ("<host>.local.") as
/// uncompressed labels.
RecordKey srvKey(std::uint16_t port, std::string_view target);
/// A TXT record's key as the advertiser sends it: one record holding every string, each length-prefixed, in order.
RecordKey txtKey(const std::vector<std::string>& txt);
/// An A record's key: the four bytes of `address` (network byte order).
RecordKey addressKey(std::uint32_t address);

/// Who wins two simultaneous probes for one name (section 8.2).
enum class Tiebreak {
	ours,   // ours is later: it keeps probing
	theirs, // theirs is later: it waits lostTiebreakWait, then probes again
	same,   // identical sets: no conflict (its own probe heard back, or an identical responder)
};
/// Section 8.2: each set sorted, then compared record by record; the first difference decides, and a set that runs out
/// first, its records all equal to the other's, loses.
Tiebreak tiebreak(std::vector<RecordKey> ours, std::vector<RecordKey> theirs);

/// Section 8.1's limit after clashes: once more than 15 clashes fall in any 10 s, each further round of probing waits
/// 5 s before its first probe's random wait.
class ConflictPace {
public:
	using Clock = std::chrono::steady_clock;
	/// A clash at `at`: how long the next round of probing waits first (0 or 5 s).
	std::chrono::milliseconds clashed(Clock::time_point at);

private:
	std::deque<Clock::time_point> recent_; // the clashes of the last 10 s
};

} // namespace ear::mdns::probing
