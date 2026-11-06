// The one user of mjansson/mdns (docs/design/sendspin.md sections 2 and 11): no daemon, no probing (the instance is unique
// by its client id), one address, the one the kernel routes the mDNS group through.
#include "advertiser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <mdns.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ear::sendspin {
namespace {

constexpr std::size_t maxNodeBytes = 50;
constexpr std::size_t maxIdBytes = 8;
constexpr std::size_t packetBytes = 2048;
/// One answer() call, every 10 ms at most, handles no more than this, so a flood cannot hold the worker up.
constexpr int maxPacketsPerAnswer = 32;
constexpr std::string_view serviceType = "_sendspin._tcp.local.";
constexpr std::string_view allServices = "_services._dns-sd._udp.local.";
constexpr std::string_view path = "/sendspin";
constexpr std::string_view pathKey = "path";
constexpr std::string_view nameKey = "name";
constexpr const char* mdnsGroup = "224.0.0.251";

std::string lastError() { return std::system_category().message(errno); }

mdns_string_t text(std::string_view from) noexcept { return {from.data(), from.size()}; }

bool sameName(mdns_string_t asked, std::string_view name) noexcept {
	return std::ranges::equal(std::string_view(asked.str, asked.length), name, [](char a, char b) {
		return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
	});
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

} // namespace

std::string advertisedInstance(std::string_view node, std::string_view clientId) {
	return "ear-" + std::string(node.substr(0, maxNodeBytes)) + "-" + std::string(clientId.substr(0, maxIdBytes));
}

struct Advertiser::State {
	State(const std::string& instance, std::string friendlyName, std::uint16_t port, in_addr address)
	    : qualifiedInstance(instance + "." + std::string(serviceType)), host(instance + ".local."), name(std::move(friendlyName)) {
		pointer.name = text(serviceType);
		pointer.type = MDNS_RECORDTYPE_PTR;
		pointer.data.ptr.name = text(qualifiedInstance);

		service.name = text(qualifiedInstance);
		service.type = MDNS_RECORDTYPE_SRV;
		service.data.srv.name = text(host);
		service.data.srv.port = port;

		hostAddress.name = text(host);
		hostAddress.type = MDNS_RECORDTYPE_A;
		hostAddress.data.a.addr.sin_family = AF_INET;
		hostAddress.data.a.addr.sin_addr = address;

		// The library joins consecutive TXT records into one, with both strings.
		txtPath.name = text(qualifiedInstance);
		txtPath.type = MDNS_RECORDTYPE_TXT;
		txtPath.data.txt.key = text(pathKey);
		txtPath.data.txt.value = text(path);
		txtName = txtPath;
		txtName.data.txt.key = text(nameKey);
		txtName.data.txt.value = text(name);
	}
	State(const State&) = delete;
	State& operator=(const State&) = delete;
	~State() {
		if (socket >= 0) ::close(socket);
	}


	std::string qualifiedInstance;
	std::string host;
	std::string name;
	mdns_record_t pointer{};
	mdns_record_t service{};
	mdns_record_t hostAddress{};
	mdns_record_t txtPath{};
	mdns_record_t txtName{};
	int socket = -1;
	alignas(std::uint32_t) std::array<char, packetBytes> received{};
	alignas(std::uint32_t) std::array<char, packetBytes> sending{};
};

namespace {

/// Sends one answer. Multicast, whatever the question's "unicast" bit: another socket on this host sharing port 5353 may
/// be handed a unicast reply, and a multicast one reaches everybody (RFC 6762 section 5.4 allows it). A query from any
/// other port is a one-shot querier, which only a unicast reply reaches (section 6.7).
void reply(int socket, const sockaddr* from, std::size_t fromLength, std::uint16_t queryId, std::uint16_t type, mdns_string_t asked,
           char* sending, const mdns_record_t& answer, const mdns_record_t* additional, std::size_t additionalCount) noexcept {
	sockaddr_in source{};
	std::memcpy(&source, from, std::min(fromLength, sizeof source));
	if (ntohs(source.sin_port) == MDNS_PORT) {
		mdns_query_answer_multicast(socket, sending, packetBytes, answer, nullptr, 0, additional, additionalCount);
	} else {
		mdns_query_answer_unicast(socket, from, fromLength, sending, packetBytes, queryId, static_cast<mdns_record_type_t>(type), asked.str,
		                          asked.length, answer, nullptr, 0, additional, additionalCount);
	}
}

} // namespace

std::expected<std::unique_ptr<Advertiser>, std::string> Advertiser::open(std::string instance, std::string name, std::uint16_t port) {
	const auto address = addressTowardsGroup();
	if (!address) return std::unexpected(address.error());
	auto state = std::make_unique<State>(instance, std::move(name), port, *address);

	state->socket = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (state->socket < 0) return std::unexpected("cannot open a socket: " + lastError());
	sockaddr_in bound{};
	bound.sin_family = AF_INET;
	bound.sin_port = htons(MDNS_PORT);
	bound.sin_addr = *address;
	if (mdns_socket_setup_ipv4(state->socket, &bound) != 0) {
		return std::unexpected("cannot join " + std::string(mdnsGroup) + " on port " + std::to_string(MDNS_PORT) + ": " + lastError());
	}

	const std::array<mdns_record_t, 4> details{state->service, state->hostAddress, state->txtPath, state->txtName};
	if (mdns_announce_multicast(state->socket, state->sending.data(), packetBytes, state->pointer, nullptr, 0, details.data(), details.size()) != 0) {
		return std::unexpected("cannot announce: " + lastError());
	}
	return std::unique_ptr<Advertiser>(new Advertiser(std::move(state)));
}

Advertiser::Advertiser(std::unique_ptr<State> state) : state_(std::move(state)) {}

Advertiser::~Advertiser() {
	const std::array<mdns_record_t, 4> details{state_->service, state_->hostAddress, state_->txtPath, state_->txtName};
	mdns_goodbye_multicast(state_->socket, state_->sending.data(), packetBytes, state_->pointer, nullptr, 0, details.data(), details.size());
}

void Advertiser::answer() noexcept {
	static constexpr mdns_record_callback_fn onRecord = [](int socket, const sockaddr* from, std::size_t fromLength, mdns_entry_type_t entry,
	                                                       std::uint16_t queryId, std::uint16_t type, std::uint16_t, std::uint32_t,
	                                                       const void* data, std::size_t size, std::size_t nameOffset, std::size_t,
	                                                       std::size_t, std::size_t, void* self) -> int {
		if (entry != MDNS_ENTRYTYPE_QUESTION) return 0;
		State& state = *static_cast<State*>(self);
		std::array<char, 256> nameBuffer;
		std::size_t offset = nameOffset;
		const mdns_string_t asked = mdns_string_extract(data, size, &offset, nameBuffer.data(), nameBuffer.size());
		const bool any = type == MDNS_RECORDTYPE_ANY;
		const auto send = [&](const mdns_record_t& answer, const auto& additional) {
			reply(socket, from, fromLength, queryId, type, asked, state.sending.data(), answer, additional.data(), additional.size());
		};
		if (sameName(asked, allServices) && (any || type == MDNS_RECORDTYPE_PTR)) {
			mdns_record_t types{};
			types.name = asked;
			types.type = MDNS_RECORDTYPE_PTR;
			types.data.ptr.name = text(serviceType);
			send(types, std::array<mdns_record_t, 0>{});
		} else if (sameName(asked, serviceType) && (any || type == MDNS_RECORDTYPE_PTR)) {
			send(state.pointer, std::array{state.service, state.hostAddress, state.txtPath, state.txtName});
		} else if (sameName(asked, state.qualifiedInstance) && (any || type == MDNS_RECORDTYPE_SRV || type == MDNS_RECORDTYPE_TXT)) {
			// The SRV answers a question about the TXT too: a multicast answer carries no question, and the TXT is with it.
			send(state.service, std::array{state.hostAddress, state.txtPath, state.txtName});
		} else if (sameName(asked, state.host) && (any || type == MDNS_RECORDTYPE_A)) {
			send(state.hostAddress, std::array<mdns_record_t, 0>{});
		}
		return 0;
	};
	for (int packets = 0; packets < maxPacketsPerAnswer; ++packets) {
		pollfd ready{state_->socket, POLLIN, 0};
		if (::poll(&ready, 1, 0) <= 0) return;
		mdns_socket_listen(state_->socket, state_->received.data(), packetBytes, onRecord, state_.get());
	}
}

} // namespace ear::sendspin
