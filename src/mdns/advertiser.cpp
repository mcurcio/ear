// The one user of mjansson/mdns (docs/node-types.md#mdns): no daemon, no probing (the caller makes the instance and
// host unique), one address, the one the kernel routes the mDNS group through.
#include "ear/mdns/advertiser.hpp"

#include "probing.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <mdns.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ear::mdns {
namespace {

constexpr std::size_t packetBytes = 2048;
/// mdns_socket_listen reads a question's type and class with no bound, up to 5 bytes past the end of what it was given,
/// and into the next member of State. So it is told less than the buffer holds.
constexpr std::size_t listenBytes = packetBytes - 8;
constexpr std::size_t headerBytes = 12;
/// Flags of a query: QR, opcode and rcode all zero. Anything else is ignored (RFC 6762 section 18), an answer above all.
constexpr std::uint16_t notAQuery = 0xF80F;
/// One answer() call reads no more than this, so a flood cannot hold its owner's loop up.
constexpr int maxPacketsPerAnswer = 32;
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

std::string lastError() { return std::system_category().message(errno); }

mdns_string_t text(std::string_view from) noexcept { return {from.data(), from.size()}; }

bool sameName(mdns_string_t asked, std::string_view name) noexcept {
	return std::ranges::equal(std::string_view(asked.str, asked.length), name, [](char a, char b) {
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

} // namespace

std::optional<std::string> listingRefused(const Listing& listing) {
	if (!validService(listing.service))
		return "a service is _<name>._tcp or _<name>._udp, its name letters, digits and -";
	if (listing.instance.empty() || listing.instance.size() > maxLabelBytes || !printableUtf8(listing.instance))
		return "an instance is 1 to 63 bytes of UTF-8, no control character";
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
	using Clock = std::chrono::steady_clock;

	State(Listing from, in_addr address, in_addr netmask)
		: listing(std::move(from)), local(address), mask(netmask),
		  serviceName(listing.service + std::string(localDomain)),
		  qualifiedInstance(listing.instance + "." + serviceName), hostName(listing.host + std::string(localDomain)) {
		pointer.name = text(serviceName);
		pointer.type = MDNS_RECORDTYPE_PTR;
		pointer.data.ptr.name = text(qualifiedInstance);

		service.name = text(qualifiedInstance);
		service.type = MDNS_RECORDTYPE_SRV;
		service.data.srv.name = text(hostName);
		service.data.srv.port = listing.port;

		hostAddress.name = text(hostName);
		hostAddress.type = MDNS_RECORDTYPE_A;
		hostAddress.data.a.addr.sin_family = AF_INET;
		hostAddress.data.a.addr.sin_addr = address;
		buildTxt();
	}
	State(const State&) = delete;
	State& operator=(const State&) = delete;
	~State() {
		if (socket >= 0) ::close(socket);
	}

	/// The TXT records, one per string, each split at its first "=" (the library joins consecutive TXT records into
	/// one, with every string), and the sets each answer carries after its own record. They point into listing.txt, so
	/// they are built again whenever it changes.
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

	/// Whether `answer` may go out by multicast now. Taking it counts the records that go with it, so a question for the
	/// service right after one for the pointer does not send the service twice in the second.
	bool mayMulticast(Answer answer, Clock::time_point at) noexcept {
		const auto recent = [&](Answer each) {
			return lastMulticast[each] && at - *lastMulticast[each] < multicastInterval;
		};
		if (recent(answer)) return false;
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
	/// Every record by multicast, the cache-flush bit set on all but the PTR (RFC 6762 section 8.3).
	bool announce() noexcept {
		if (mdns_announce_multicast(socket, sending.data(), packetBytes, pointer, nullptr, 0, withPointer.data(),
		                            withPointer.size()) != 0)
			return false;
		stampMulticast(pointerAndAll, Clock::now());
		return true;
	}

	Listing listing;
	in_addr local;
	in_addr mask;
	std::array<bool, answerCount> answered{}; // in this datagram
	std::array<std::optional<Clock::time_point>, answerCount> lastMulticast{};
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
	alignas(std::uint32_t) std::array<char, packetBytes> received{};
	alignas(std::uint32_t) std::array<char, packetBytes> sending{};
};

namespace {

/// Sends one answer, at most once per datagram and within the rates above. Multicast, whatever the question's "unicast"
/// bit: another socket on this host sharing port 5353 may be handed a unicast reply, and a multicast one reaches
/// everybody (RFC 6762 section 5.4 allows it). A query from any other port is a one-shot querier, which only a unicast
/// reply reaches (RFC 6762 section 6.7).
void reply(Advertiser::State& state, Advertiser::State::Answer which, int socket, const sockaddr* from,
           std::size_t fromLength, std::uint16_t queryId, std::uint16_t type, mdns_string_t asked,
           const mdns_record_t& answer, const mdns_record_t* additional, std::size_t additionalCount) noexcept {
	if (std::exchange(state.answered[which], true)) return;
	const auto at = Advertiser::State::Clock::now();
	sockaddr_in source{};
	std::memcpy(&source, from, std::min(fromLength, sizeof source));
	if (ntohs(source.sin_port) == MDNS_PORT) {
		if (state.mayMulticast(which, at))
			mdns_query_answer_multicast(socket, state.sending.data(), packetBytes, answer, nullptr, 0, additional,
			                            additionalCount);
	} else if (state.mayUnicast(at)) {
		mdns_query_answer_unicast(socket, from, fromLength, state.sending.data(), packetBytes, queryId,
		                          static_cast<mdns_record_type_t>(type), asked.str, asked.length, answer, nullptr, 0,
		                          additional, additionalCount);
	}
}

/// Takes a datagram off the socket unread, unless it is a query from this link. Looked at first because mdns_socket_listen
/// hears every datagram the same way, and answers questions in a response too. A datagram too short to be a query is dropped
/// here, too: the library reads its header from the last one.
bool queryIsNext(const Advertiser::State& state, int socket) noexcept {
	std::array<std::uint8_t, headerBytes> header{};
	sockaddr_in from{};
	socklen_t fromLength = sizeof from;
	const auto got =
		::recvfrom(socket, header.data(), header.size(), MSG_PEEK, reinterpret_cast<sockaddr*>(&from), &fromLength);
	if (got < 0) return false;
	const auto flags = static_cast<std::uint16_t>((header[2] << 8U) | header[3]);
	const bool query = static_cast<std::size_t>(got) == headerBytes && (flags & notAQuery) == 0 &&
	                   onLink(from.sin_addr.s_addr, state.local.s_addr, state.mask.s_addr);
	if (!query) ::recv(socket, header.data(), header.size(), 0);
	return query;
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
	if (mdns_socket_setup_ipv4(state->socket, &bound) != 0) {
		return std::unexpected("cannot join " + std::string(mdnsGroup) + " on port " + std::to_string(MDNS_PORT) +
		                       ": " + lastError());
	}
	if (!state->announce()) return std::unexpected("cannot announce: " + lastError());
	return std::unique_ptr<Advertiser>(new Advertiser(std::move(state)));
}

Advertiser::Advertiser(std::unique_ptr<State> state) : state_(std::move(state)) {}

Advertiser::~Advertiser() {
	mdns_goodbye_multicast(state_->socket, state_->sending.data(), packetBytes, state_->pointer, nullptr, 0,
	                       state_->withPointer.data(), state_->withPointer.size());
}

int Advertiser::descriptor() const noexcept { return state_->socket; }

const Listing& Advertiser::listing() const noexcept { return state_->listing; }

std::expected<void, std::string> Advertiser::update(std::vector<std::string> txt) {
	if (auto why = txtRefused(txt)) return std::unexpected(std::move(*why));
	std::swap(state_->listing.txt, txt);
	state_->buildTxt();
	if (state_->announce()) return {};
	const std::string why = "cannot announce: " + lastError();
	std::swap(state_->listing.txt, txt);
	state_->buildTxt();
	return std::unexpected(why);
}

void Advertiser::answer() noexcept {
	static constexpr mdns_record_callback_fn onRecord =
		[](int socket, const sockaddr* from, std::size_t fromLength, mdns_entry_type_t entry, std::uint16_t queryId,
	       std::uint16_t type, std::uint16_t, std::uint32_t, const void* data, std::size_t size, std::size_t nameOffset,
	       std::size_t, std::size_t, std::size_t, void* self) -> int {
		if (entry != MDNS_ENTRYTYPE_QUESTION) return 0;
		State& state = *static_cast<State*>(self);
		std::array<char, 256> nameBuffer;
		std::size_t offset = nameOffset;
		const mdns_string_t asked = mdns_string_extract(data, size, &offset, nameBuffer.data(), nameBuffer.size());
		const bool any = type == MDNS_RECORDTYPE_ANY;
		const auto send = [&](State::Answer which, const mdns_record_t& answer, const auto& additional) {
			reply(state, which, socket, from, fromLength, queryId, type, asked, answer, additional.data(),
			      additional.size());
		};
		if (sameName(asked, allServices) && (any || type == MDNS_RECORDTYPE_PTR)) {
			mdns_record_t types{};
			types.name = asked;
			types.type = MDNS_RECORDTYPE_PTR;
			types.data.ptr.name = text(state.serviceName);
			send(State::types, types, std::array<mdns_record_t, 0>{});
		} else if (sameName(asked, state.serviceName) && (any || type == MDNS_RECORDTYPE_PTR)) {
			send(State::pointerAndAll, state.pointer, state.withPointer);
		} else if (sameName(asked, state.qualifiedInstance) &&
		           (any || type == MDNS_RECORDTYPE_SRV || type == MDNS_RECORDTYPE_TXT)) {
			// The SRV answers a question about the TXT too: a multicast answer carries no question, and the TXT is with it.
			send(State::serviceAndAll, state.service, state.withService);
		} else if (sameName(asked, state.hostName) && (any || type == MDNS_RECORDTYPE_A)) {
			send(State::addressOnly, state.hostAddress, std::array<mdns_record_t, 0>{});
		}
		return 0;
	};
	for (int packets = 0; packets < maxPacketsPerAnswer; ++packets) {
		pollfd ready{state_->socket, POLLIN, 0};
		if (::poll(&ready, 1, 0) <= 0) return;
		if (!queryIsNext(*state_, state_->socket)) continue;
		state_->answered = {};
		mdns_socket_listen(state_->socket, state_->received.data(), listenBytes, onRecord, state_.get());
	}
}

} // namespace ear::mdns
