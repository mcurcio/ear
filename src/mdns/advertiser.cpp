// The one user of mjansson/mdns (docs/node-types.md#mdns): no daemon, one address (the one the kernel routes the mDNS
// group through), and RFC 6762's probing and renaming (sections 8 and 9) written here over the library's record
// writers, since it has no probe of its own. The pure parts (names, keys, the tiebreak, the pace) are probing.hpp's.
#include "ear/mdns/advertiser.hpp"

#include "probing.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <mdns.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace ear::mdns {
namespace {

constexpr std::size_t packetBytes = 2048;
constexpr std::size_t headerBytes = 12;
/// The header's QR bit: set in a response, clear in a query.
constexpr std::uint16_t responseFlag = 0x8000;
/// Opcode and rcode: a datagram with either set is ignored (RFC 6762 section 18).
constexpr std::uint16_t opcodeOrRcode = 0x780F;
/// One turn reads no more than this, so a flood cannot hold its owner's loop up.
constexpr int maxPacketsPerTurn = 32;
/// RFC 6762 section 6: a record goes out by multicast at most once a second.
constexpr std::chrono::seconds multicastInterval{1};
/// Unicast has no such rule, but each reply goes wherever a datagram's source says, which a peer can forge. One-shot queriers
/// are rare and ask again, so this many a second is plenty, and it bounds what can be reflected.
constexpr int unicastRepliesPerSecond = 16;
constexpr std::string_view allServices = "_services._dns-sd._udp.local.";
constexpr std::string_view localDomain = ".local.";
constexpr const char* mdnsGroup = "224.0.0.251";
constexpr std::size_t maxLabelBytes = 63;
constexpr std::size_t maxTxtBytes = 255;
constexpr std::uint16_t classIn = MDNS_CLASS_IN;
constexpr std::uint16_t classMask = 0x7FFF; // the cache-flush bit cleared
constexpr std::uint32_t recordTtl = 60;     // what mjansson/mdns announces with

std::string lastError() { return std::system_category().message(errno); }

mdns_string_t text(std::string_view from) noexcept { return {from.data(), from.size()}; }

bool sameName(std::string_view asked, std::string_view name) noexcept {
	return std::ranges::equal(asked, name, [](char a, char b) {
		return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
	});
}

bool isLetterOrDigit(char c) noexcept {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/// `_<name>._tcp` or `_<name>._udp`, its name letters, digits and "-" (RFC 6335 section 5.1's characters).
bool validService(std::string_view service) noexcept {
	if (!service.ends_with("._tcp") && !service.ends_with("._udp")) return false;
	const std::string_view name = service.substr(0, service.size() - 5);
	return name.size() >= 2 && name.front() == '_' &&
	       std::ranges::all_of(name.substr(1), [](char c) { return isLetterOrDigit(c) || c == '-'; });
}

/// Whether `text` is RFC 3629's UTF-8 (no overlong form, no surrogate, nothing above U+10FFFF) holding no C0, DEL, C1
/// or Unicode line or paragraph separator, which a browser's list or a log would act on.
bool printableUtf8(std::string_view text) noexcept {
	for (std::size_t at = 0; at < text.size();) {
		const auto lead = static_cast<unsigned char>(text[at]);
		std::size_t continuation = 0;
		unsigned char low = 0x80;
		unsigned char high = 0xBF;
		char32_t code = 0;
		if (lead < 0x80) {
			code = lead;
		} else if (lead >= 0xC2 && lead <= 0xDF) {
			continuation = 1;
			code = lead & 0x1FU;
		} else if (lead >= 0xE0 && lead <= 0xEF) {
			continuation = 2;
			code = lead & 0x0FU;
			if (lead == 0xE0) low = 0xA0;
			if (lead == 0xED) high = 0x9F;
		} else if (lead >= 0xF0 && lead <= 0xF4) {
			continuation = 3;
			code = lead & 0x07U;
			if (lead == 0xF0) low = 0x90;
			if (lead == 0xF4) high = 0x8F;
		} else {
			return false;
		}
		if (at + continuation >= text.size() && continuation > 0) return false;
		for (std::size_t c = 1; c <= continuation; ++c) {
			const auto byte = static_cast<unsigned char>(text[at + c]);
			if (byte < low || byte > high) return false;
			code = (code << 6U) | (byte & 0x3FU);
			low = 0x80;
			high = 0xBF;
		}
		if (code < 0x20 || (code >= 0x7F && code <= 0x9F) || code == 0x2028 || code == 0x2029) return false;
		at += continuation + 1;
	}
	return true;
}

std::optional<std::string> txtRefused(std::string_view txt) {
	if (txt.empty() || txt.size() > maxTxtBytes) return "a TXT string is 1 to 255 bytes";
	const auto equals = txt.find('=');
	if (equals == std::string_view::npos) return "a TXT string needs key=value";
	const std::string_view key = txt.substr(0, equals);
	if (key.empty() || !std::ranges::all_of(key, [](char c) { return c >= 0x20 && c <= 0x7E; }))
		return "a TXT string's key is 1 or more of printable ASCII";
	return std::nullopt;
}

std::optional<std::string> txtRefused(const std::vector<std::string>& txt) {
	for (const auto& each : txt) {
		if (auto why = txtRefused(each)) return why;
	}
	return std::nullopt;
}

/// The address the kernel would send to the mDNS group from: a connected datagram socket sends nothing.
std::expected<in_addr, std::string> addressTowardsGroup() {
	const int probe = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (probe < 0) return std::unexpected("cannot open a socket: " + lastError());
	sockaddr_in group{};
	group.sin_family = AF_INET;
	group.sin_port = htons(MDNS_PORT);
	::inet_pton(AF_INET, mdnsGroup, &group.sin_addr);
	sockaddr_in local{};
	socklen_t length = sizeof local;
	const bool found = ::connect(probe, reinterpret_cast<const sockaddr*>(&group), sizeof group) == 0 &&
	                   ::getsockname(probe, reinterpret_cast<sockaddr*>(&local), &length) == 0;
	const std::string why = found ? "" : lastError();
	::close(probe);
	if (!found) return std::unexpected(std::string("no route to ") + mdnsGroup + ": " + why);
	return local.sin_addr;
}

/// The netmask of the interface holding `address`; a host's own /32 when none says.
in_addr netmaskOf(in_addr address) noexcept {
	in_addr mask{};
	mask.s_addr = INADDR_NONE;
	ifaddrs* interfaces = nullptr;
	if (::getifaddrs(&interfaces) != 0) return mask;
	for (const ifaddrs* each = interfaces; each != nullptr; each = each->ifa_next) {
		if (each->ifa_addr == nullptr || each->ifa_netmask == nullptr || each->ifa_addr->sa_family != AF_INET) continue;
		if (reinterpret_cast<const sockaddr_in*>(each->ifa_addr)->sin_addr.s_addr != address.s_addr) continue;
		mask = reinterpret_cast<const sockaddr_in*>(each->ifa_netmask)->sin_addr;
		break;
	}
	::freeifaddrs(interfaces);
	return mask;
}

std::uint16_t readShort(const char* at) noexcept {
	return static_cast<std::uint16_t>((static_cast<unsigned char>(at[0]) << 8U) | static_cast<unsigned char>(at[1]));
}

/// A wait on the advertiser's timerfd, `delay` from now; never 0, which would disarm it.
bool arm(int timer, std::chrono::nanoseconds delay) noexcept {
	delay = std::max(delay, std::chrono::nanoseconds{1});
	itimerspec when{};
	when.it_value.tv_sec = static_cast<time_t>(std::chrono::duration_cast<std::chrono::seconds>(delay).count());
	when.it_value.tv_nsec = static_cast<long>((delay % std::chrono::seconds(1)).count());
	return ::timerfd_settime(timer, 0, &when, nullptr) == 0;
}

/// Section 8.1's random wait before a round's first probe, so hosts started together do not probe in step.
std::chrono::milliseconds firstProbeWait() {
	thread_local std::mt19937 random{std::random_device{}()};
	return std::chrono::milliseconds(
		std::uniform_int_distribution<std::int64_t>(0, probing::firstProbeMost.count())(random));
}

} // namespace

std::optional<std::string> listingRefused(const Listing& listing) {
	if (!validService(listing.service))
		return "a service is _<name>._tcp or _<name>._udp, its name letters, digits and -";
	if (listing.instance.empty() || listing.instance.size() > maxLabelBytes || !printableUtf8(listing.instance) ||
	    listing.instance.find('.') != std::string::npos)
		return "an instance is 1 to 63 bytes of UTF-8, no control character and no .";
	if (listing.host.empty() || listing.host.size() > maxLabelBytes ||
	    !std::ranges::all_of(listing.host, [](char c) { return isLetterOrDigit(c) || c == '-' || c == '_'; }))
		return "a host is 1 to 63 of letters, digits, - and _";
	if (listing.port == 0) return "a port is 1 to 65535";
	return txtRefused(listing.txt);
}

bool onLink(std::uint32_t sourceAddress, std::uint32_t localAddress, std::uint32_t netmask) noexcept {
	const std::uint32_t source = ntohl(sourceAddress);
	const bool loopback = (source >> 24U) == 127;
	return loopback || (sourceAddress & netmask) == (localAddress & netmask);
}

struct Advertiser::State {
	/// What can be asked for: each goes out once per datagram, and by multicast once a second.
	enum Answer { types, pointerAndAll, serviceAndAll, addressOnly, answerCount };
	/// Where it is in RFC 6762's round: probing (and waiting to), announced (holding its names), or failed.
	enum class Phase { probing, announced, failed };
	/// What the timerfd's next expiry does.
	enum class Step { none, probe, announce, secondAnnouncement };
	using Clock = std::chrono::steady_clock;

	/// One record a datagram carried, as section 8.2 compares it: its name, its class with the cache-flush bit cleared,
	/// its type, its TTL and its rdata uncompressed.
	struct Heard {
		std::string name;
		std::uint32_t ttl = 0;
		bool authority = false;
		probing::RecordKey key;
	};
	struct Question {
		std::string name;
		std::uint16_t type = 0;
	};

	State(Listing from, in_addr address, in_addr netmask)
		: listing(std::move(from)), askedInstance(listing.instance), askedHost(listing.host), local(address),
		  mask(netmask) {
		buildRecords();
	}
	State(const State&) = delete;
	State& operator=(const State&) = delete;
	~State() {
		for (const int each : {epoll, timer, socket})
			if (each >= 0) ::close(each);
	}

	/// The names and the records, built again whenever a name or the TXT changes: the records point into the names and
	/// into listing.txt.
	void buildRecords() {
		serviceName = listing.service + std::string(localDomain);
		qualifiedInstance = listing.instance + "." + serviceName;
		hostName = listing.host + std::string(localDomain);
		pointer = {};
		pointer.name = text(serviceName);
		pointer.type = MDNS_RECORDTYPE_PTR;
		pointer.data.ptr.name = text(qualifiedInstance);

		service = {};
		service.name = text(qualifiedInstance);
		service.type = MDNS_RECORDTYPE_SRV;
		service.data.srv.name = text(hostName);
		service.data.srv.port = listing.port;

		hostAddress = {};
		hostAddress.name = text(hostName);
		hostAddress.type = MDNS_RECORDTYPE_A;
		hostAddress.data.a.addr.sin_family = AF_INET;
		hostAddress.data.a.addr.sin_addr = local;
		buildTxt();
	}

	/// The TXT records, one per string, each split at its first "=" (the library joins consecutive TXT records into
	/// one, with every string), and the sets each answer carries after its own record.
	void buildTxt() {
		txt.clear();
		for (const std::string& each : listing.txt) {
			const std::string_view whole = each;
			const auto equals = whole.find('=');
			mdns_record_t record{};
			record.name = text(qualifiedInstance);
			record.type = MDNS_RECORDTYPE_TXT;
			record.data.txt.key = text(whole.substr(0, equals));
			record.data.txt.value = text(whole.substr(equals + 1));
			txt.push_back(record);
		}
		withPointer = {service, hostAddress};
		withPointer.insert(withPointer.end(), txt.begin(), txt.end());
		withService = {hostAddress};
		withService.insert(withService.end(), txt.begin(), txt.end());
	}

	/// The unique records it holds at `name` as section 8.2 compares them: the SRV and the TXT at its instance, the A at
	/// its host; none at any other name. A listing with no TXT string sends no TXT record.
	std::vector<probing::RecordKey> keysAt(std::string_view name) const {
		std::vector<probing::RecordKey> keys;
		if (sameName(name, qualifiedInstance)) {
			keys.push_back(probing::srvKey(listing.port, hostName));
			if (!listing.txt.empty()) keys.push_back(probing::txtKey(listing.txt));
		}
		if (sameName(name, hostName)) keys.push_back(probing::addressKey(local.s_addr));
		return keys;
	}

	/// Whether `heard` is a record of a name it probes or holds, of a type and class it has there, with other rdata: a
	/// conflict (sections 8.1 and 9). A goodbye (TTL 0) withdraws a record and is none.
	bool conflicts(const Heard& heard) const {
		if (heard.ttl == 0 || ownEarlier(heard.key)) return false;
		for (const probing::RecordKey& ours : keysAt(heard.name))
			if (ours.type == heard.key.type && ours.rrclass == heard.key.rrclass && ours.rdata != heard.key.rdata)
				return true;
		return false;
	}

	/// Whether `key` is a TXT this advertiser sent at its names before its last update: its own probe or announcement
	/// heard back after it changed, never another host's.
	bool ownEarlier(const probing::RecordKey& key) const {
		return key.type == MDNS_RECORDTYPE_TXT && std::ranges::find(earlierTxt, key.rdata) != earlierTxt.end();
	}
	/// Keeps `before`, the TXT an update replaced, as its own (above); forgotten at a rename, under which it never went.
	void rememberTxt(const std::vector<std::string>& before) {
		constexpr std::size_t kept = 16;
		if (before == listing.txt) return;
		std::string rdata = probing::txtKey(before).rdata;
		if (std::ranges::find(earlierTxt, rdata) != earlierTxt.end()) return;
		if (earlierTxt.size() == kept) earlierTxt.erase(earlierTxt.begin());
		earlierTxt.push_back(std::move(rdata));
	}

	/// Whether `answer` may go out by multicast now. Taking it counts the records that go with it, so a question for the
	/// service right after one for the pointer does not send the service twice in the second.
	bool mayMulticast(Answer answer, Clock::time_point at) noexcept {
		if (lastMulticast[answer] && at - *lastMulticast[answer] < multicastInterval) return false;
		stampMulticast(answer, at);
		return true;
	}
	void stampMulticast(Answer answer, Clock::time_point at) noexcept {
		lastMulticast[answer] = at;
		if (answer == pointerAndAll) lastMulticast[serviceAndAll] = at;
		if (answer == pointerAndAll || answer == serviceAndAll) lastMulticast[addressOnly] = at;
	}
	bool mayUnicast(Clock::time_point at) noexcept {
		if (at - unicastWindow >= std::chrono::seconds(1)) {
			unicastWindow = at;
			unicastSent = 0;
		}
		return unicastSent < unicastRepliesPerSecond && ++unicastSent > 0;
	}

	/// Every record by multicast, the cache-flush bit set on all but the PTR (section 8.3).
	bool announce() noexcept {
		if (mdns_announce_multicast(socket, sending.data(), packetBytes, pointer, nullptr, 0, withPointer.data(),
		                            withPointer.size()) != 0)
			return false;
		stampMulticast(pointerAndAll, Clock::now());
		return true;
	}
	/// Every record it announced, again with TTL 0 (section 10.1).
	void goodbye() noexcept {
		mdns_goodbye_multicast(socket, sending.data(), packetBytes, pointer, nullptr, 0, withPointer.data(),
		                       withPointer.size());
	}

	/// One probe (section 8.1): a query asking both its names with type ANY, class IN and the unicast-response bit
	/// clear (port 5353 is shared, so a unicast reply could reach a neighbour's socket), its authority section the
	/// SRV, TXT and A it proposes.
	bool probe() noexcept {
		auto* buffer = sending.data();
		const auto header = [&](std::size_t at, std::uint16_t value) { mdns_htons(buffer + at, value); };
		header(0, 0); // id
		header(2, 0); // a query
		header(4, 2);
		header(6, 0);
		header(8, static_cast<std::uint16_t>(listing.txt.empty() ? 2 : 3));
		header(10, 0);
		mdns_string_table_t table{};
		void* data = buffer + headerBytes;
		for (const std::string* name : {&qualifiedInstance, &hostName}) {
			data = mdns_string_make(buffer, packetBytes, data, name->data(), name->size(), &table);
			if (data == nullptr || packetBytes - static_cast<std::size_t>(static_cast<char*>(data) - buffer) < 4)
				return false;
			data = mdns_htons(data, MDNS_RECORDTYPE_ANY);
			data = mdns_htons(data, classIn);
		}
		for (mdns_record_t record : {service, hostAddress}) {
			record.rclass = classIn;
			record.ttl = recordTtl;
			data = mdns_answer_add_record(buffer, packetBytes, data, record, &table);
		}
		data =
			mdns_answer_add_txt_record(buffer, packetBytes, data, txt.data(), txt.size(), classIn, recordTtl, &table);
		if (data == nullptr) return false;
		return mdns_multicast_send(socket, buffer, static_cast<std::size_t>(static_cast<char*>(data) - buffer)) == 0;
	}

	/// A new round of probing, its first probe `wait` from now.
	void startProbing(std::chrono::nanoseconds wait) noexcept {
		phase = Phase::probing;
		probesSent = 0;
		next = Step::probe;
		arm(timer, wait);
	}

	Listing listing;
	std::string askedInstance;
	std::string askedHost;
	unsigned instanceNumber = 1; // the last "(n)" given; 1: none
	unsigned hostNumber = 1;
	in_addr local;
	in_addr mask;
	Phase phase = Phase::probing;
	Step next = Step::none;
	int probesSent = 0;
	bool holding = false;                // its records are announced under the names it has: a goodbye owed
	std::vector<std::string> earlierTxt; // the TXT rdata it sent at its names before an update (ownEarlier)
	probing::ConflictPace pace;
	std::array<bool, answerCount> answered{}; // in this datagram
	std::array<std::optional<Clock::time_point>, answerCount> lastMulticast{};
	std::array<std::optional<Clock::time_point>, answerCount> lastProbeAnswer{};
	Clock::time_point unicastWindow{};
	int unicastSent = 0;
	std::string serviceName;
	std::string qualifiedInstance;
	std::string hostName;
	mdns_record_t pointer{};
	mdns_record_t service{};
	mdns_record_t hostAddress{};
	std::vector<mdns_record_t> txt;
	std::vector<mdns_record_t> withPointer; // the SRV, the A and the TXT
	std::vector<mdns_record_t> withService; // the A and the TXT
	int socket = -1;
	int timer = -1;
	int epoll = -1;
	alignas(std::uint32_t) std::array<char, packetBytes> received{};
	alignas(std::uint32_t) std::array<char, packetBytes> sending{};
};

namespace {

using State = Advertiser::State;

/// Sends one answer, at most once per datagram and within the rates above. Multicast, whatever the question's "unicast"
/// bit: another socket on this host sharing port 5353 may be handed a unicast reply, and a multicast one reaches
/// everybody (RFC 6762 section 5.4 allows it). A query from any other port is a one-shot querier, which only a unicast
/// reply reaches (RFC 6762 section 6.7).
void reply(State& state, State::Answer which, const sockaddr_in& from, std::uint16_t queryId, std::uint16_t type,
           std::string_view asked, const mdns_record_t& answer, const mdns_record_t* additional,
           std::size_t additionalCount) noexcept {
	if (std::exchange(state.answered[which], true)) return;
	const auto at = State::Clock::now();
	if (ntohs(from.sin_port) == MDNS_PORT) {
		if (state.mayMulticast(which, at))
			mdns_query_answer_multicast(state.socket, state.sending.data(), packetBytes, answer, nullptr, 0, additional,
			                            additionalCount);
	} else if (state.mayUnicast(at)) {
		mdns_query_answer_unicast(state.socket, &from, sizeof from, state.sending.data(), packetBytes, queryId,
		                          static_cast<mdns_record_type_t>(type), asked.data(), asked.size(), answer, nullptr, 0,
		                          additional, additionalCount);
	}
}

/// A question asked once announced: answered from what it holds (above).
void answerQuestion(State& state, const sockaddr_in& from, std::uint16_t queryId, const State::Question& question) {
	const bool any = question.type == MDNS_RECORDTYPE_ANY;
	const auto send = [&](State::Answer which, const mdns_record_t& answer, const auto& additional) {
		reply(state, which, from, queryId, question.type, question.name, answer, additional.data(), additional.size());
	};
	if (sameName(question.name, allServices) && (any || question.type == MDNS_RECORDTYPE_PTR)) {
		mdns_record_t types{};
		types.name = text(question.name);
		types.type = MDNS_RECORDTYPE_PTR;
		types.data.ptr.name = text(state.serviceName);
		send(State::types, types, std::array<mdns_record_t, 0>{});
	} else if (sameName(question.name, state.serviceName) && (any || question.type == MDNS_RECORDTYPE_PTR)) {
		send(State::pointerAndAll, state.pointer, state.withPointer);
	} else if (sameName(question.name, state.qualifiedInstance) &&
	           (any || question.type == MDNS_RECORDTYPE_SRV || question.type == MDNS_RECORDTYPE_TXT)) {
		// The SRV answers a question about the TXT too: a multicast answer carries no question, and the TXT is with it.
		send(State::serviceAndAll, state.service, state.withService);
	} else if (sameName(question.name, state.hostName) && (any || question.type == MDNS_RECORDTYPE_A)) {
		send(State::addressOnly, state.hostAddress, std::array<mdns_record_t, 0>{});
	}
}

/// A probe for a name it holds, answered by multicast at once (section 8.1: the prober must hear it within its 250 ms),
/// exempt from the one-second rule but at most once per probeAnswerInterval per record set (section 6).
void answerProbe(State& state, const State::Question& question) {
	const auto at = State::Clock::now();
	const auto send = [&](State::Answer which, const mdns_record_t& answer, const auto& additional) {
		if (std::exchange(state.answered[which], true)) return;
		if (state.lastProbeAnswer[which] && at - *state.lastProbeAnswer[which] < probing::probeAnswerInterval) return;
		state.lastProbeAnswer[which] = at;
		mdns_query_answer_multicast(state.socket, state.sending.data(), packetBytes, answer, nullptr, 0,
		                            additional.data(), additional.size());
	};
	if (sameName(question.name, state.qualifiedInstance)) send(State::serviceAndAll, state.service, state.withService);
	else if (sameName(question.name, state.hostName))
		send(State::addressOnly, state.hostAddress, std::array<mdns_record_t, 0>{});
}

/// A name of `size` bytes at `offset` of `packet`, which moves past it; nothing when it does not parse.
std::optional<std::string> nameAt(const char* packet, std::size_t size, std::size_t& offset) {
	std::array<char, 256> buffer{};
	std::size_t end = offset;
	if (mdns_string_skip(packet, size, &end) == 0) return std::nullopt;
	std::size_t from = offset;
	const mdns_string_t name = mdns_string_extract(packet, size, &from, buffer.data(), buffer.size());
	offset = end;
	return std::string(name.str, name.length);
}

/// One record's rdata, uncompressed as section 8.2 compares it: an SRV's target name expanded, any other's bytes as
/// sent (a TXT's and an A's hold no name).
std::optional<std::string> rdataOf(const char* packet, std::uint16_t type, std::size_t offset, std::size_t length) {
	if (type != MDNS_RECORDTYPE_SRV) return std::string(packet + offset, length);
	constexpr std::size_t fixed = 6; // priority, weight, port
	if (length < fixed + 1) return std::nullopt;
	std::size_t at = offset + fixed;
	const auto target = nameAt(packet, offset + length, at);
	if (!target) return std::nullopt;
	std::string rdata = probing::srvKey(readShort(packet + offset + 4), *target).rdata;
	std::memcpy(rdata.data(), packet + offset, 4); // its own priority and weight
	return rdata;
}

/// A datagram from this link, parsed; nothing when any part does not parse (it is then ignored whole).
struct Datagram {
	std::uint16_t id = 0;
	std::uint16_t flags = 0;
	std::vector<State::Question> questions;
	std::vector<State::Heard> records;
	bool probe = false; // a query with an authority section
};

std::optional<Datagram> parse(const char* packet, std::size_t size) {
	if (size < headerBytes) return std::nullopt;
	Datagram datagram{.id = readShort(packet), .flags = readShort(packet + 2), .questions = {}, .records = {}};
	const std::uint16_t questions = readShort(packet + 4);
	const std::uint16_t answers = readShort(packet + 6);
	const std::uint16_t authorities = readShort(packet + 8);
	const std::uint16_t additionals = readShort(packet + 10);
	std::size_t offset = headerBytes;
	for (std::uint16_t i = 0; i < questions; ++i) {
		auto name = nameAt(packet, size, offset);
		if (!name || offset + 4 > size) return std::nullopt;
		datagram.questions.push_back({std::move(*name), readShort(packet + offset)});
		offset += 4;
	}
	const std::size_t records = std::size_t{answers} + authorities + additionals;
	for (std::size_t i = 0; i < records; ++i) {
		auto name = nameAt(packet, size, offset);
		constexpr std::size_t fixed = 10; // type, class, TTL, length
		if (!name || offset + fixed > size) return std::nullopt;
		const std::uint16_t type = readShort(packet + offset);
		const auto rrclass = static_cast<std::uint16_t>(readShort(packet + offset + 2) & classMask);
		const std::uint32_t ttl =
			(std::uint32_t{readShort(packet + offset + 4)} << 16U) | readShort(packet + offset + 6);
		const std::uint16_t length = readShort(packet + offset + 8);
		offset += fixed;
		if (length > size - offset) return std::nullopt;
		auto rdata = rdataOf(packet, type, offset, length);
		if (!rdata) return std::nullopt;
		offset += length;
		const bool authority = i >= answers && i < std::size_t{answers} + authorities;
		datagram.records.push_back({.name = std::move(*name),
		                            .ttl = ttl,
		                            .authority = authority,
		                            .key = {.rrclass = rrclass, .type = type, .rdata = std::move(*rdata)}});
	}
	datagram.probe = (datagram.flags & responseFlag) == 0 && authorities > 0;
	return datagram;
}

/// A clash in a response while probing (section 8.1), or once announced (section 9): each clashing name renamed, after
/// a goodbye for what it announced under the old names, then probing again after the pace's wait.
void onConflict(State& state, const Datagram& datagram) {
	bool instance = false;
	bool host = false;
	for (const State::Heard& heard : datagram.records) {
		if (!state.conflicts(heard)) continue;
		if (sameName(heard.name, state.qualifiedInstance)) instance = true;
		else host = true;
	}
	if (!instance && !host) return;
	if (state.phase == State::Phase::announced) {
		// Section 9: back to probing under the same names; renamed only if it then loses.
		state.startProbing(firstProbeWait());
		return;
	}
	if (std::exchange(state.holding, false)) state.goodbye();
	if (instance) state.listing.instance = probing::instanceNamed(state.askedInstance, ++state.instanceNumber);
	if (host) state.listing.host = probing::hostNamed(state.askedHost, ++state.hostNumber);
	state.earlierTxt.clear();
	state.buildRecords();
	state.startProbing(state.pace.clashed(State::Clock::now()) + firstProbeWait());
}

/// Another host's probe heard while probing (section 8.2): for each name both probe, the later set of records wins; the
/// loser waits, then probes the same names again.
void onSimultaneousProbe(State& state, const Datagram& datagram) {
	for (const std::string* name : {&state.qualifiedInstance, &state.hostName}) {
		std::vector<probing::RecordKey> theirs;
		for (const State::Heard& heard : datagram.records)
			if (heard.authority && sameName(heard.name, *name) && !state.ownEarlier(heard.key))
				theirs.push_back(heard.key);
		if (theirs.empty()) continue;
		if (probing::tiebreak(state.keysAt(*name), std::move(theirs)) == probing::Tiebreak::theirs) {
			state.startProbing(probing::lostTiebreakWait);
			return;
		}
	}
}

/// One datagram off the socket, heard by the phase it is in. Nothing is answered while probing.
void hear(State& state, const char* packet, std::size_t size, const sockaddr_in& from) {
	if (!onLink(from.sin_addr.s_addr, state.local.s_addr, state.mask.s_addr)) return;
	const auto datagram = parse(packet, size);
	if (!datagram || (datagram->flags & opcodeOrRcode) != 0) return;
	if ((datagram->flags & responseFlag) != 0) {
		onConflict(state, *datagram);
		return;
	}
	if (state.phase == State::Phase::probing) {
		if (datagram->probe) onSimultaneousProbe(state, *datagram);
		return;
	}
	state.answered = {};
	for (const State::Question& question : datagram->questions) {
		if (datagram->probe) answerProbe(state, question);
		else answerQuestion(state, from, datagram->id, question);
	}
}

/// The step the timerfd says is due; the words of a send that failed.
std::optional<std::string> step(State& state, std::optional<Listing>& announced) {
	switch (std::exchange(state.next, State::Step::none)) {
	case State::Step::none:
		return std::nullopt;
	case State::Step::probe:
		if (!state.probe()) return "cannot probe: " + lastError();
		++state.probesSent;
		state.next = state.probesSent < probing::probeCount ? State::Step::probe : State::Step::announce;
		arm(state.timer, probing::probeInterval);
		return std::nullopt;
	case State::Step::announce:
		if (!state.announce()) return "cannot announce: " + lastError();
		state.phase = State::Phase::announced;
		state.holding = true;
		announced = state.listing;
		state.next = State::Step::secondAnnouncement;
		arm(state.timer, probing::announceInterval);
		return std::nullopt;
	case State::Step::secondAnnouncement:
		if (!state.announce()) return "cannot announce: " + lastError();
		return std::nullopt;
	}
	return std::nullopt;
}

} // namespace

std::expected<std::unique_ptr<Advertiser>, std::string> Advertiser::open(Listing listing) {
	if (auto why = listingRefused(listing)) return std::unexpected(std::move(*why));
	const auto address = addressTowardsGroup();
	if (!address) return std::unexpected(address.error());
	auto state = std::make_unique<State>(std::move(listing), *address, netmaskOf(*address));

	state->socket = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (state->socket < 0) return std::unexpected("cannot open a socket: " + lastError());
	sockaddr_in bound{};
	bound.sin_family = AF_INET;
	bound.sin_port = htons(MDNS_PORT);
	bound.sin_addr = *address;
	if (mdns_socket_setup_ipv4(state->socket, &bound) != 0) { // it leaves the socket non-blocking
		return std::unexpected("cannot join " + std::string(mdnsGroup) + " on port " + std::to_string(MDNS_PORT) +
		                       ": " + lastError());
	}
	state->timer = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (state->timer < 0) return std::unexpected("cannot open a timer: " + lastError());
	state->epoll = ::epoll_create1(EPOLL_CLOEXEC);
	if (state->epoll < 0) return std::unexpected("cannot open an epoll: " + lastError());
	for (const int each : {state->socket, state->timer}) {
		epoll_event watch{};
		watch.events = EPOLLIN;
		watch.data.fd = each;
		if (::epoll_ctl(state->epoll, EPOLL_CTL_ADD, each, &watch) != 0)
			return std::unexpected("cannot open an epoll: " + lastError());
	}
	state->startProbing(firstProbeWait());
	return std::unique_ptr<Advertiser>(new Advertiser(std::move(state)));
}

Advertiser::Advertiser(std::unique_ptr<State> state) : state_(std::move(state)) {}

Advertiser::~Advertiser() {
	if (state_->holding) state_->goodbye();
}

int Advertiser::descriptor() const noexcept { return state_->epoll; }

const Listing& Advertiser::listing() const noexcept { return state_->listing; }

std::expected<void, std::string> Advertiser::update(std::vector<std::string> txt) {
	if (auto why = txtRefused(txt)) return std::unexpected(std::move(*why));
	std::swap(state_->listing.txt, txt);
	state_->buildTxt();
	if (state_->phase != State::Phase::announced || state_->announce()) {
		state_->rememberTxt(txt);
		return {};
	}
	const std::string why = "cannot announce: " + lastError();
	std::swap(state_->listing.txt, txt);
	state_->buildTxt();
	return std::unexpected(why);
}

Advertiser::Turn Advertiser::turn() noexcept {
	State& state = *state_;
	Turn turn;
	if (state.phase == State::Phase::failed) return turn;
	for (int packets = 0; packets < maxPacketsPerTurn; ++packets) {
		sockaddr_in from{};
		socklen_t fromLength = sizeof from;
		const auto got = ::recvfrom(state.socket, state.received.data(), state.received.size(), 0,
		                            reinterpret_cast<sockaddr*>(&from), &fromLength);
		if (got < 0) break; // nothing waits (or the socket failed, which the next send says)
		try {
			hear(state, state.received.data(), static_cast<std::size_t>(got), from);
		} catch (const std::bad_alloc&) { // a name's string: this datagram goes unheard
		}
	}
	std::uint64_t expiries = 0;
	if (::read(state.timer, &expiries, sizeof expiries) != static_cast<ssize_t>(sizeof expiries)) return turn;
	try {
		if (auto failed = step(state, turn.announced)) {
			state.phase = State::Phase::failed;
			turn.announced.reset();
			turn.failed = std::move(failed);
		}
	} catch (const std::bad_alloc&) {
		state.phase = State::Phase::failed;
		turn = {};
		turn.failed = "cannot announce: out of memory";
	}
	return turn;
}

} // namespace ear::mdns
