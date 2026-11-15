// A node's mDNS listing (docs/node-types.md#mdns), over real sockets on the address the kernel routes the mDNS group
// through: what a one-shot querier is answered, what a TXT question gets after an update, and the goodbye a browser
// hears. Each case lists a service of its own, so cases side by side, and sendspin's system tests, answer only their
// own. Answers are read with mjansson/mdns, as a browser built on it would.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <mdns.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/mdns/advertiser.hpp"

using ear::mdns::Advertiser;
using ear::mdns::Listing;
using ear::mdns::listingRefused;
using ear::mdns::onLink;

namespace {

/// Only fails a test: every wait below ends at the event it waits for.
constexpr std::chrono::seconds deadline{10};

std::uint32_t address(const char* text) {
	in_addr parsed{};
	REQUIRE(inet_pton(AF_INET, text, &parsed) == 1);
	return parsed.s_addr;
}

/// The address the kernel sends to the mDNS group from, which an advertiser's A record carries.
in_addr addressTowardsGroup() {
	const int probe = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	REQUIRE(probe >= 0);
	sockaddr_in group{};
	group.sin_family = AF_INET;
	group.sin_port = htons(MDNS_PORT);
	group.sin_addr.s_addr = address("224.0.0.251");
	sockaddr_in local{};
	socklen_t length = sizeof local;
	const bool found = ::connect(probe, reinterpret_cast<const sockaddr*>(&group), sizeof group) == 0 &&
	                   ::getsockname(probe, reinterpret_cast<sockaddr*>(&local), &length) == 0;
	::close(probe);
	REQUIRE(found);
	return local.sin_addr;
}

Listing listingFor(std::string service) {
	return Listing{.service = std::move(service),
	               .instance = "ear-test-instance",
	               .host = "ear-test-host",
	               .port = 4321,
	               .txt = {"path=/test", "name=Front Room", "eq=a=b"}};
}

/// One record a response carried, as mjansson/mdns reads it.
struct Record {
	mdns_entry_type_t section = MDNS_ENTRYTYPE_QUESTION;
	std::uint16_t type = 0;
	std::uint32_t ttl = 0;
	std::string name;
	std::string target;           // PTR and SRV
	std::uint16_t port = 0;       // SRV
	in_addr address{};            // A
	std::vector<std::string> txt; // TXT, each "key=value"
};

int collect(int, const sockaddr*, std::size_t, mdns_entry_type_t section, std::uint16_t, std::uint16_t type,
            std::uint16_t, std::uint32_t ttl, const void* data, std::size_t size, std::size_t nameOffset, std::size_t,
            std::size_t recordOffset, std::size_t recordLength, void* into) {
	std::array<char, 256> buffer{};
	Record record;
	record.section = section;
	record.type = type;
	record.ttl = ttl;
	std::size_t offset = nameOffset;
	const mdns_string_t name = mdns_string_extract(data, size, &offset, buffer.data(), buffer.size());
	record.name.assign(name.str, name.length);
	if (type == MDNS_RECORDTYPE_PTR) {
		const mdns_string_t target =
			mdns_record_parse_ptr(data, size, recordOffset, recordLength, buffer.data(), buffer.size());
		record.target.assign(target.str, target.length);
	} else if (type == MDNS_RECORDTYPE_SRV) {
		const mdns_record_srv_t service =
			mdns_record_parse_srv(data, size, recordOffset, recordLength, buffer.data(), buffer.size());
		record.target.assign(service.name.str, service.name.length);
		record.port = service.port;
	} else if (type == MDNS_RECORDTYPE_A) {
		sockaddr_in parsed{};
		record.address = mdns_record_parse_a(data, size, recordOffset, recordLength, &parsed)->sin_addr;
	} else if (type == MDNS_RECORDTYPE_TXT) {
		std::array<mdns_record_txt_t, 16> strings{};
		const std::size_t count =
			mdns_record_parse_txt(data, size, recordOffset, recordLength, strings.data(), strings.size());
		for (std::size_t i = 0; i < count; ++i) {
			record.txt.push_back(std::string(strings[i].key.str, strings[i].key.length) + "=" +
			                     std::string(strings[i].value.str, strings[i].value.length));
		}
	}
	static_cast<std::vector<Record>*>(into)->push_back(std::move(record));
	return 0;
}

/// A UDP socket that hears answers. Not joined: on a free port, a one-shot querier, whose questions go to the group and
/// whose answers come back to it alone. Joined: on port 5353 and in the group, as a browser is.
class MdnsSocket {
public:
	explicit MdnsSocket(bool joined) {
		if (joined) {
			sockaddr_in any{};
			any.sin_family = AF_INET;
			any.sin_port = htons(MDNS_PORT);
			any.sin_addr.s_addr = htonl(INADDR_ANY);
			fd_ = mdns_socket_open_ipv4(&any);
		} else {
			fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		}
		REQUIRE(fd_ >= 0);
	}
	~MdnsSocket() { ::close(fd_); }
	MdnsSocket(const MdnsSocket&) = delete;
	MdnsSocket& operator=(const MdnsSocket&) = delete;

	/// Asks the group about `name`, by multicast, as mjansson/mdns's one-shot query does (the unicast bit set, since
	/// this socket is not on port 5353). Returns the query's id.
	std::uint16_t ask(mdns_record_type_t type, std::string_view name) {
		const std::uint16_t id = nextId_++;
		std::array<char, 512> buffer{};
		REQUIRE(mdns_query_send(fd_, type, name.data(), name.size(), buffer.data(), buffer.size(), id) == id);
		return id;
	}

	/// The records of the next response (QR set) that `wanted` accepts, read as they arrive; nothing past the
	/// deadline. `id` 0 takes any response, a browser's view; otherwise only the answer to that query. With `serving`,
	/// that advertiser answers whenever its descriptor is readable, as its owner's loop would.
	template <typename Wanted>
	std::optional<std::vector<Record>> nextResponse(int id, Wanted wanted, Advertiser* serving = nullptr) {
		const auto until = std::chrono::steady_clock::now() + deadline;
		std::array<char, 9000> buffer{};
		for (;;) {
			const auto left =
				std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
			if (left <= 0) return std::nullopt;
			std::array<pollfd, 2> ready{pollfd{fd_, POLLIN, 0}, pollfd{-1, POLLIN, 0}};
			if (serving != nullptr) ready[1].fd = serving->descriptor();
			if (::poll(ready.data(), ready.size(), static_cast<int>(left)) <= 0) continue;
			if (serving != nullptr && (ready[1].revents & POLLIN) != 0) serving->answer();
			if ((ready[0].revents & POLLIN) == 0) continue;
			const auto peeked = ::recv(fd_, buffer.data(), 4, MSG_PEEK);
			if (peeked < 4 || (static_cast<unsigned char>(buffer[2]) & 0x80U) == 0) {
				::recv(fd_, buffer.data(), buffer.size(), 0); // a query, or a runt: not an answer
				continue;
			}
			std::vector<Record> records;
			mdns_query_recv(fd_, buffer.data(), buffer.size(), collect, &records, id);
			if (!records.empty() && wanted(records)) return records;
		}
	}

private:
	int fd_ = -1;
	std::uint16_t nextId_ = 0x1E57;
};

constexpr auto anyAnswer = [](const std::vector<Record>&) { return true; };

const Record* find(const std::vector<Record>& records, std::uint16_t type, std::string_view name) {
	const auto at =
		std::ranges::find_if(records, [&](const Record& each) { return each.type == type && each.name == name; });
	return at == records.end() ? nullptr : &*at;
}

std::string qualified(const Listing& listing) { return listing.instance + "." + listing.service + ".local."; }

} // namespace

TEST_CASE("A query is answered only from this link: the local subnet, or this host itself", "[mdns][advertiser]") {
	const std::uint32_t local = address("192.168.1.10");
	const std::uint32_t mask = address("255.255.255.0");
	CHECK(onLink(address("192.168.1.77"), local, mask));
	CHECK(onLink(address("192.168.1.10"), local, mask));
	CHECK(onLink(address("127.0.0.1"), local, mask));
	CHECK(onLink(address("127.200.0.3"), local, mask));
	CHECK_FALSE(onLink(address("192.168.2.77"), local, mask));
	CHECK_FALSE(onLink(address("8.8.8.8"), local, mask));
	// With no netmask known, only the host's own address.
	CHECK(onLink(local, local, 0xFFFFFFFFU));
	CHECK_FALSE(onLink(address("192.168.1.77"), local, 0xFFFFFFFFU));
}

TEST_CASE("A listing is refused for the first rule it breaks, in words", "[mdns][advertiser]") {
	const Listing good = listingFor("_ear-refused._tcp");
	CHECK_FALSE(listingRefused(good));
	CHECK_FALSE(listingRefused([&] {
		Listing udp = good;
		udp.service = "_x._udp";
		udp.instance = std::string(63, 'i');
		udp.host = std::string(63, 'h');
		udp.port = 65535;
		udp.txt = {"k=", std::string(253, 'v').insert(0, "k=")};
		return udp;
	}()));
	CHECK_FALSE(listingRefused([&] {
		Listing unicode = good;
		unicode.instance = "Küche \xF0\x9F\x8E\xB5";
		unicode.host = "ear-Back_room-AbC_9-x";
		unicode.txt = {};
		return unicode;
	}()));

	const auto refusal = [&](auto change) {
		Listing listing = good;
		change(listing);
		const auto why = listingRefused(listing);
		return why.value_or("");
	};
	const std::string service = "a service is _<name>._tcp or _<name>._udp, its name letters, digits and -";
	CHECK(refusal([](Listing& l) { l.service = ""; }) == service);
	CHECK(refusal([](Listing& l) { l.service = "_ear._sctp"; }) == service);
	CHECK(refusal([](Listing& l) { l.service = "ear._tcp"; }) == service);
	CHECK(refusal([](Listing& l) { l.service = "__tcp"; }) == service);
	CHECK(refusal([](Listing& l) { l.service = "_._tcp"; }) == service);
	CHECK(refusal([](Listing& l) { l.service = "_e.r._tcp"; }) == service);
	CHECK(refusal([](Listing& l) { l.service = "_ear._tcp.local"; }) == service);

	const std::string instance = "an instance is 1 to 63 bytes of UTF-8, no control character";
	CHECK(refusal([](Listing& l) { l.instance = ""; }) == instance);
	CHECK(refusal([](Listing& l) { l.instance = std::string(64, 'i'); }) == instance);
	CHECK(refusal([](Listing& l) { l.instance = "bad\xC3"; }) == instance);
	CHECK(refusal([](Listing& l) { l.instance = "over\xC0\xAFlong"; }) == instance);
	CHECK(refusal([](Listing& l) { l.instance = "tab\there"; }) == instance);
	CHECK(refusal([](Listing& l) { l.instance = "del\x7F"; }) == instance);
	CHECK(refusal([](Listing& l) { l.instance = "c1\xC2\x85"; }) == instance);

	const std::string host = "a host is 1 to 63 of letters, digits, - and _";
	CHECK(refusal([](Listing& l) { l.host = ""; }) == host);
	CHECK(refusal([](Listing& l) { l.host = std::string(64, 'h'); }) == host);
	CHECK(refusal([](Listing& l) { l.host = "a.b"; }) == host);
	CHECK(refusal([](Listing& l) { l.host = "sp ace"; }) == host);

	CHECK(refusal([](Listing& l) { l.port = 0; }) == "a port is 1 to 65535");

	const std::string txt = "a TXT string is 1 to 255 bytes";
	CHECK(refusal([](Listing& l) { l.txt = {"a=b", ""}; }) == txt);
	CHECK(refusal([](Listing& l) { l.txt = {std::string(254, 'v').insert(0, "k=")}; }) == txt);
	CHECK(refusal([](Listing& l) { l.txt = {"bare"}; }) == "a TXT string needs key=value");
	CHECK(refusal([](Listing& l) { l.txt = {"=value"}; }) == "a TXT string's key is 1 or more of printable ASCII");
	CHECK(refusal([](Listing& l) { l.txt = {"k\x01y=value"}; }) ==
	      "a TXT string's key is 1 or more of printable ASCII");
	CHECK(refusal([](Listing& l) { l.txt = {"k\xC3\xA9=value"}; }) ==
	      "a TXT string's key is 1 or more of printable ASCII");

	// The first rule broken is the one named.
	CHECK(refusal([](Listing& l) {
			  l.service = "";
			  l.port = 0;
		  }) == service);

	auto refused = Advertiser::open([&] {
		Listing listing = good;
		listing.port = 0;
		return listing;
	}());
	REQUIRE_FALSE(refused);
	CHECK(refused.error() == "a port is 1 to 65535");
}

TEST_CASE("A one-shot querier asking for the service gets its PTR, SRV, TXT and A", "[mdns][advertiser]") {
	const Listing listing = listingFor("_ear-oneshot._tcp");
	auto advertiser = Advertiser::open(listing);
	REQUIRE(advertiser);
	CHECK((*advertiser)->listing() == listing);

	MdnsSocket querier(false);
	const std::uint16_t id = querier.ask(MDNS_RECORDTYPE_PTR, listing.service + ".local.");
	const auto answer = querier.nextResponse(id, anyAnswer, advertiser->get());
	REQUIRE(answer);

	const Record* pointer = find(*answer, MDNS_RECORDTYPE_PTR, listing.service + ".local.");
	REQUIRE(pointer != nullptr);
	CHECK(pointer->section == MDNS_ENTRYTYPE_ANSWER);
	CHECK(pointer->target == qualified(listing));
	const Record* service = find(*answer, MDNS_RECORDTYPE_SRV, qualified(listing));
	REQUIRE(service != nullptr);
	CHECK(service->target == listing.host + ".local.");
	CHECK(service->port == listing.port);
	const Record* txt = find(*answer, MDNS_RECORDTYPE_TXT, qualified(listing));
	REQUIRE(txt != nullptr);
	CHECK(txt->txt == listing.txt);
	const Record* host = find(*answer, MDNS_RECORDTYPE_A, listing.host + ".local.");
	REQUIRE(host != nullptr);
	CHECK(host->address.s_addr == addressTowardsGroup().s_addr);
}

TEST_CASE("After an update a TXT question gets the new strings, and a refused update changes nothing",
          "[mdns][advertiser]") {
	const Listing listing = listingFor("_ear-update._tcp");
	auto advertiser = Advertiser::open(listing);
	REQUIRE(advertiser);

	const auto refused = (*advertiser)->update({"bare"});
	REQUIRE_FALSE(refused);
	CHECK(refused.error() == "a TXT string needs key=value");
	CHECK((*advertiser)->listing() == listing);

	const std::vector<std::string> fresh = {"path=/other", "name=Back Room"};
	REQUIRE((*advertiser)->update(fresh));
	CHECK((*advertiser)->listing().txt == fresh);

	MdnsSocket querier(false);
	const std::uint16_t id = querier.ask(MDNS_RECORDTYPE_TXT, qualified(listing));
	const auto answer = querier.nextResponse(id, anyAnswer, advertiser->get());
	REQUIRE(answer);
	const Record* txt = find(*answer, MDNS_RECORDTYPE_TXT, qualified(listing));
	REQUIRE(txt != nullptr);
	CHECK(txt->txt == fresh);
}

TEST_CASE("Destroying an advertiser sends its records with TTL 0, which a browser hears", "[mdns][advertiser]") {
	const Listing listing = listingFor("_ear-goodbye._tcp");
	MdnsSocket browser(true); // before the advertiser: it hears the announcement, then the goodbye
	auto advertiser = Advertiser::open(listing);
	REQUIRE(advertiser);
	const auto ours = [&](std::uint32_t ttl) {
		return [&listing, ttl](const std::vector<Record>& records) {
			const Record* pointer = find(records, MDNS_RECORDTYPE_PTR, listing.service + ".local.");
			return pointer != nullptr && pointer->target == qualified(listing) && pointer->ttl == ttl;
		};
	};
	REQUIRE(browser.nextResponse(0, ours(60)));

	advertiser->reset();
	const auto goodbye = browser.nextResponse(0, ours(0));
	REQUIRE(goodbye);
	for (const std::uint16_t type : {MDNS_RECORDTYPE_SRV, MDNS_RECORDTYPE_TXT}) {
		const Record* record = find(*goodbye, type, qualified(listing));
		REQUIRE(record != nullptr);
		CHECK(record->ttl == 0);
	}
	const Record* host = find(*goodbye, MDNS_RECORDTYPE_A, listing.host + ".local.");
	REQUIRE(host != nullptr);
	CHECK(host->ttl == 0);
}
