// The pure parts of probing and renaming a listing (probing.hpp; RFC 6762 sections 8 and 9, docs/node-types.md#mdns).
#include "probing.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace ear::mdns::probing {
namespace {

constexpr std::size_t maxLabelBytes = 63;
constexpr std::uint16_t typeA = 1;
constexpr std::uint16_t typeTxt = 16;
constexpr std::uint16_t typeSrv = 33;
constexpr auto clashWindow = std::chrono::seconds(10);
constexpr std::size_t clashesBeforePacing = 15;
constexpr std::chrono::milliseconds pacedWait{5'000};

void appendShort(std::string& to, std::uint16_t value) {
	to += static_cast<char>((value >> 8U) & 0xFFU);
	to += static_cast<char>(value & 0xFFU);
}

/// `name` ("a.b.local.", its last dot optional) as uncompressed DNS labels, each length-prefixed, then the root's 0.
std::string labels(std::string_view name) {
	if (name.ends_with('.')) name.remove_suffix(1);
	std::string out;
	while (!name.empty()) {
		const std::size_t dot = std::min(name.find('.'), name.size());
		out += static_cast<char>(dot);
		out += name.substr(0, dot);
		name.remove_prefix(std::min(dot + 1, name.size()));
	}
	out += '\0';
	return out;
}

} // namespace

std::string instanceNamed(std::string_view asked, unsigned n) {
	const std::string suffix = " (" + std::to_string(n) + ")";
	std::size_t cut = std::min(asked.size(), maxLabelBytes - suffix.size());
	// Never inside a character: back up over continuation bytes (10xxxxxx) to the start of the one cut through.
	while (cut > 0 && cut < asked.size() && (static_cast<unsigned char>(asked[cut]) & 0xC0U) == 0x80U) --cut;
	return std::string(asked.substr(0, cut)) + suffix;
}

std::string hostNamed(std::string_view asked, unsigned n) {
	const std::string suffix = "-" + std::to_string(n);
	return std::string(asked.substr(0, std::min(asked.size(), maxLabelBytes - suffix.size()))) + suffix;
}

RecordKey srvKey(std::uint16_t port, std::string_view target) {
	RecordKey key{.rrclass = 1, .type = typeSrv, .rdata = {}};
	appendShort(key.rdata, 0); // priority
	appendShort(key.rdata, 0); // weight
	appendShort(key.rdata, port);
	key.rdata += labels(target);
	return key;
}

RecordKey txtKey(const std::vector<std::string>& txt) {
	RecordKey key{.rrclass = 1, .type = typeTxt, .rdata = {}};
	for (const std::string& each : txt) {
		key.rdata += static_cast<char>(each.size());
		key.rdata += each;
	}
	return key;
}

RecordKey addressKey(std::uint32_t address) {
	RecordKey key{.rrclass = 1, .type = typeA, .rdata = {}};
	key.rdata.resize(sizeof address);
	std::memcpy(key.rdata.data(), &address, sizeof address);
	return key;
}

Tiebreak tiebreak(std::vector<RecordKey> ours, std::vector<RecordKey> theirs) {
	std::ranges::sort(ours);
	std::ranges::sort(theirs);
	const std::size_t common = std::min(ours.size(), theirs.size());
	for (std::size_t i = 0; i < common; ++i) {
		// std::string compares its bytes as unsigned char, as section 8.2 asks.
		const auto order = ours[i] <=> theirs[i];
		if (order < 0) return Tiebreak::theirs;
		if (order > 0) return Tiebreak::ours;
	}
	if (ours.size() == theirs.size()) return Tiebreak::same;
	return ours.size() < theirs.size() ? Tiebreak::theirs : Tiebreak::ours;
}

std::chrono::milliseconds ConflictPace::clashed(Clock::time_point at) {
	while (!recent_.empty() && at - recent_.front() >= clashWindow) recent_.pop_front();
	recent_.push_back(at);
	return recent_.size() > clashesBeforePacing ? pacedWait : std::chrono::milliseconds{0};
}

} // namespace ear::mdns::probing
