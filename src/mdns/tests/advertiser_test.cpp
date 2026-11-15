// A node's mDNS listing (docs/node-types.md#mdns), over real sockets on the address the kernel routes the mDNS group
// through: its probes and announcements, the renames a clash makes, what a one-shot querier is answered, what a TXT
// question gets after an update, and the goodbye a browser hears. Each case lists a service of its own, so cases side by
// side, and sendspin's system tests, clash only where they mean to. Answers are read with mjansson/mdns, as a browser
// built on it would. Every advertiser is turned whenever its descriptor is readable, as its owner's loop turns it.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <mdns.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/mdns/advertiser.hpp"
#include "probing.hpp"

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
	int fd() const noexcept { return fd_; }

	/// `packet` to the group, as any responder or prober on the link sends it.
	void send(const std::string& packet) {
		sockaddr_in group{};
		group.sin_family = AF_INET;
		group.sin_port = htons(MDNS_PORT);
		group.sin_addr.s_addr = address("224.0.0.251");
		REQUIRE(::sendto(fd_, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&group),
		                 sizeof group) == static_cast<ssize_t>(packet.size()));
	}

	/// One datagram that waits, whatever it is: its header, its bytes and its records; nothing when none waits.
	struct Datagram {
		std::uint16_t id = 0;
		bool response = false;
		std::uint16_t authorities = 0;
		std::string bytes;
		std::vector<Record> records;
	};
	std::optional<Datagram> read() {
		std::array<char, 9000> buffer{};
		const auto got = ::recv(fd_, buffer.data(), buffer.size(), MSG_PEEK | MSG_DONTWAIT);
		if (got < 0) return std::nullopt;
		if (got < 12) {
			::recv(fd_, buffer.data(), buffer.size(), 0); // a runt
			return std::nullopt;
		}
		Datagram datagram;
		datagram.bytes.assign(buffer.data(), static_cast<std::size_t>(got));
		const auto word = [&](std::size_t at) {
			return static_cast<std::uint16_t>((static_cast<unsigned char>(buffer[at]) << 8U) |
			                                  static_cast<unsigned char>(buffer[at + 1]));
		};
		datagram.id = word(0);
		datagram.response = (word(2) & 0x8000U) != 0;
		datagram.authorities = word(8);
		mdns_query_recv(fd_, buffer.data(), buffer.size(), collect, &datagram.records, 0);
		return datagram;
	}

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
			if (serving != nullptr && (ready[1].revents & POLLIN) != 0) serving->turn();
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

/// An advertiser as its owner's loop keeps it: turned when readable, each announcement it reports kept.
struct Served {
	explicit Served(const Listing& listing) {
		auto opened = Advertiser::open(listing);
		REQUIRE(opened);
		advertiser = std::move(*opened);
	}
	std::unique_ptr<Advertiser> advertiser;
	std::vector<Listing> announced;
};

/// Turns each of `served` whenever its descriptor is readable, and calls `onSocket` whenever `socket` is, until
/// `done()`; false at `patience`. A failed turn fails the test.
bool turnUntil(const std::vector<Served*>& served, const std::function<bool()>& done, int socket = -1,
               const std::function<void()>& onSocket = {}, std::chrono::seconds patience = deadline) {
	const auto until = std::chrono::steady_clock::now() + patience;
	for (;;) {
		if (done()) return true;
		const auto left =
			std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
		if (left <= 0) return false;
		std::vector<pollfd> ready;
		for (const Served* each : served) ready.push_back(pollfd{each->advertiser->descriptor(), POLLIN, 0});
		ready.push_back(pollfd{socket, POLLIN, 0});
		if (::poll(ready.data(), ready.size(), static_cast<int>(left)) <= 0) continue;
		for (std::size_t i = 0; i < served.size(); ++i) {
			if ((ready[i].revents & POLLIN) == 0) continue;
			auto turn = served[i]->advertiser->turn();
			REQUIRE_FALSE(turn.failed);
			if (turn.announced) served[i]->announced.push_back(std::move(*turn.announced));
		}
		if (socket >= 0 && (ready.back().revents & POLLIN) != 0) onSocket();
	}
}

/// Turns `served` until its first announcement.
void announce(Served& served) {
	REQUIRE(turnUntil({&served}, [&] { return !served.announced.empty(); }));
}

/// `name` as uncompressed DNS labels.
std::string labels(std::string_view name) { return ear::mdns::probing::srvKey(0, name).rdata.substr(6); }

std::string word(std::uint16_t value) {
	return {static_cast<char>((value >> 8U) & 0xFFU), static_cast<char>(value & 0xFFU)};
}

/// A record as a datagram carries it: `name` uncompressed, its type, class IN (with the cache-flush bit when `flush`),
/// TTL 120 and `rdata`.
std::string record(std::string_view name, std::uint16_t type, const std::string& rdata, bool flush = false) {
	return labels(name) + word(type) + word(flush ? 0x8001 : 0x0001) + word(0) + word(120) +
	       word(static_cast<std::uint16_t>(rdata.size())) + rdata;
}

/// Another host's probe for `instance` (its qualified name) on `host`, proposing an SRV with `port`.
std::string probeFor(std::string_view instance, std::string_view host, std::uint16_t port) {
	return word(0) + word(0) + word(1) + word(0) + word(1) + word(0) + labels(instance) + word(MDNS_RECORDTYPE_ANY) +
	       word(1) + record(instance, MDNS_RECORDTYPE_SRV, ear::mdns::probing::srvKey(port, host).rdata);
}

/// A query marked by `id`, which the sender hears back after anything it sent before it.
std::string marker(std::uint16_t id) {
	return word(id) + word(0) + word(1) + word(0) + word(0) + word(0) + labels("_ear-marker._tcp.local.") +
	       word(MDNS_RECORDTYPE_PTR) + word(1);
}

/// A query whose authority section proposes records for `name` (qualified): a probe for it.
bool isProbeFor(const MdnsSocket::Datagram& datagram, std::string_view name) {
	return !datagram.response && datagram.authorities > 0 &&
	       std::ranges::any_of(datagram.records, [&](const Record& each) {
			   return each.section == MDNS_ENTRYTYPE_AUTHORITY && each.name == name;
		   });
}

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
		unicode.host = "ear-Back-room-AbC-9-x";
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

	const std::string instance = "an instance is 1 to 63 bytes of UTF-8, no control character and no .";
	CHECK(refusal([](Listing& l) { l.instance = ""; }) == instance);
	CHECK(refusal([](Listing& l) { l.instance = std::string(64, 'i'); }) == instance);
	CHECK(refusal([](Listing& l) { l.instance = "bad\xC3"; }) == instance);
	CHECK(refusal([](Listing& l) { l.instance = "over\xC0\xAFlong"; }) == instance);
	CHECK(refusal([](Listing& l) { l.instance = "tab\there"; }) == instance);
	CHECK(refusal([](Listing& l) { l.instance = "del\x7F"; }) == instance);
	CHECK(refusal([](Listing& l) { l.instance = "c1\xC2\x85"; }) == instance);
	// mjansson/mdns splits a name at its dots, so an instance with one would go out as two labels.
	CHECK(refusal([](Listing& l) { l.instance = "Mr. K"; }) == instance);
	CHECK(refusal([](Listing& l) { l.instance = "."; }) == instance);

	const std::string host = "a host is 1 to 63 of letters, digits and -, not starting or ending with -";
	CHECK(refusal([](Listing& l) { l.host = ""; }) == host);
	CHECK(refusal([](Listing& l) { l.host = "ear_host"; }) == host);
	CHECK(refusal([](Listing& l) { l.host = "-ear"; }) == host);
	CHECK(refusal([](Listing& l) { l.host = "ear-"; }) == host);
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
	Served served(listing);
	CHECK(served.advertiser->listing() == listing);
	announce(served);

	MdnsSocket querier(false);
	const std::uint16_t id = querier.ask(MDNS_RECORDTYPE_PTR, listing.service + ".local.");
	const auto answer = querier.nextResponse(id, anyAnswer, served.advertiser.get());
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
	Served served(listing);
	announce(served);
	Advertiser& advertiser = *served.advertiser;

	const auto refused = advertiser.update({"bare"});
	REQUIRE_FALSE(refused);
	CHECK(refused.error() == "a TXT string needs key=value");
	CHECK(advertiser.listing() == listing);

	const std::vector<std::string> fresh = {"path=/other", "name=Back Room"};
	REQUIRE(advertiser.update(fresh));
	CHECK(advertiser.listing().txt == fresh);

	MdnsSocket querier(false);
	const std::uint16_t id = querier.ask(MDNS_RECORDTYPE_TXT, qualified(listing));
	const auto answer = querier.nextResponse(id, anyAnswer, &advertiser);
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
	// The announcement comes once its probes went unanswered: the advertiser is turned meanwhile.
	const auto ours = [&](std::uint32_t ttl) {
		return [&listing, ttl](const std::vector<Record>& records) {
			const Record* pointer = find(records, MDNS_RECORDTYPE_PTR, listing.service + ".local.");
			return pointer != nullptr && pointer->target == qualified(listing) && pointer->ttl == ttl;
		};
	};
	REQUIRE(browser.nextResponse(0, ours(60), advertiser->get()));

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

TEST_CASE("A lone advertiser probes three times, 250 ms apart, then announces once what it asked for",
          "[mdns][advertiser][probing]") {
	const Listing listing = listingFor("_ear-probes._tcp");
	MdnsSocket browser(true); // before the advertiser: it hears every probe
	Served served(listing);
	std::vector<std::chrono::steady_clock::time_point> probes;
	std::size_t probesBeforeAnnouncing = 0;
	std::size_t announcements = 0;
	const auto heard = [&] {
		while (auto datagram = browser.read()) {
			if (isProbeFor(*datagram, qualified(listing))) {
				probes.push_back(std::chrono::steady_clock::now());
				// The authority section proposes its SRV, TXT and A; a probe asks with no unicast bit (checked in the
				// question's class below).
				const Record* srv = find(datagram->records, MDNS_RECORDTYPE_SRV, qualified(listing));
				const Record* txt = find(datagram->records, MDNS_RECORDTYPE_TXT, qualified(listing));
				const Record* host = find(datagram->records, MDNS_RECORDTYPE_A, listing.host + ".local.");
				REQUIRE(srv != nullptr);
				CHECK(srv->port == listing.port);
				REQUIRE(txt != nullptr);
				CHECK(txt->txt == listing.txt);
				REQUIRE(host != nullptr);
				CHECK(host->address.s_addr == addressTowardsGroup().s_addr);
				const std::string question = labels(qualified(listing)) + word(MDNS_RECORDTYPE_ANY) + word(1);
				CHECK(datagram->bytes.find(question) != std::string::npos);
			} else if (datagram->response) {
				const Record* pointer = find(datagram->records, MDNS_RECORDTYPE_PTR, listing.service + ".local.");
				if (pointer == nullptr || pointer->target != qualified(listing) || pointer->ttl == 0) continue;
				if (announcements++ == 0) probesBeforeAnnouncing = probes.size();
			}
		}
	};
	// Until the second announcement, a second after the first.
	REQUIRE(turnUntil({&served}, [&] { return announcements >= 2; }, browser.fd(), heard));
	CHECK(probesBeforeAnnouncing == static_cast<std::size_t>(ear::mdns::probing::probeCount));
	CHECK(probes.size() == probesBeforeAnnouncing);
	for (std::size_t i = 1; i < probes.size(); ++i) CHECK(probes[i] - probes[i - 1] >= std::chrono::milliseconds(200));
	REQUIRE(served.announced.size() == 1);
	CHECK(served.announced[0] == listing);
	CHECK(served.advertiser->listing() == listing);
}

TEST_CASE("A probe for a held instance is answered at once, inside the second after the announcement",
          "[mdns][advertiser][probing]") {
	const Listing listing = listingFor("_ear-probe-answer._tcp");
	MdnsSocket prober(true);
	Served served(listing);
	announce(served);
	const auto announcedAt = std::chrono::steady_clock::now();
	prober.send(probeFor(qualified(listing), "ear-elsewhere.local.", 9));
	// Its answer holds its SRV as the answer and no PTR: not the second announcement.
	bool answered = false;
	REQUIRE(turnUntil(
		{&served}, [&] { return answered; }, prober.fd(),
		[&] {
			while (auto datagram = prober.read()) {
				if (!datagram->response) continue;
				const Record* srv = find(datagram->records, MDNS_RECORDTYPE_SRV, qualified(listing));
				if (srv == nullptr || srv->section != MDNS_ENTRYTYPE_ANSWER) continue;
				if (find(datagram->records, MDNS_RECORDTYPE_PTR, listing.service + ".local.") != nullptr) continue;
				CHECK(srv->port == listing.port);
				answered = true;
			}
		}));
	CHECK(std::chrono::steady_clock::now() - announcedAt < std::chrono::seconds(1));
}

TEST_CASE("An advertiser opened while another holds its instance on another port announces '<instance> (2)'",
          "[mdns][advertiser][probing]") {
	Listing listing = listingFor("_ear-holder._tcp");
	listing.port = 1000;
	Served holder(listing);
	announce(holder);
	Listing late = listing;
	late.port = 2000;
	Served newcomer(late);
	REQUIRE(turnUntil({&holder, &newcomer}, [&] { return !newcomer.announced.empty(); }));
	Listing renamed = late;
	renamed.instance = listing.instance + " (2)";
	CHECK(newcomer.announced[0] == renamed);
	CHECK(newcomer.advertiser->listing() == renamed);
	CHECK(holder.announced.size() == 1);
	CHECK(holder.advertiser->listing() == listing);
}

TEST_CASE("Two opened together with one instance: the later records keep it, the other becomes '(2)'",
          "[mdns][advertiser][probing]") {
	Listing low = listingFor("_ear-together._tcp");
	low.port = 1000;
	Listing high = low;
	high.port = 2000;
	Served first(low);
	Served second(high);
	REQUIRE(turnUntil({&first, &second}, [&] { return !first.announced.empty() && !second.announced.empty(); }));
	// Section 8.2: the TXT records are equal, and the SRV with port 2000 is the later.
	CHECK(second.announced[0] == high);
	Listing renamed = low;
	renamed.instance = low.instance + " (2)";
	CHECK(first.announced[0] == renamed);
}

TEST_CASE("A host label is made of any text: other characters a '-', cut to 63 bytes, no '-' at either end",
          "[mdns][advertiser]") {
	using ear::mdns::hostLabelOf;
	CHECK(hostLabelOf("ear-kitchen") == "ear-kitchen");
	CHECK(hostLabelOf("ear-back_porch-AbC_9-x") == "ear-back-porch-AbC-9-x");
	CHECK(hostLabelOf("ear-den-ab_-") == "ear-den-ab");
	CHECK(hostLabelOf("_x_") == "x");
	CHECK(hostLabelOf("___") == "ear");
	CHECK(hostLabelOf("") == "ear");
	CHECK(hostLabelOf(std::string(62, 'h') + "_x") == std::string(62, 'h'));
	CHECK(hostLabelOf(std::string(80, 'h')) == std::string(63, 'h'));
	for (const std::string& text :
	     {std::string("ear-back_porch-"), std::string(70, '_') + "y", std::string("caf\xC3\xA9 ok")}) {
		Listing listing = listingFor("_ear-host-label._tcp");
		listing.host = hostLabelOf(text);
		INFO(text);
		CHECK_FALSE(listingRefused(listing));
	}
}

TEST_CASE("A response that clashes with a name already announced sends it back to probing that name, and losing "
          "renames it",
          "[mdns][advertiser][probing]") {
	Listing listing = listingFor("_ear-late-clash._tcp");
	listing.host = "ear-test-late-clash";
	MdnsSocket responder(true);
	Served served(listing);
	announce(served);
	REQUIRE(served.announced[0] == listing);
	// From here on, another host holds the host name: it says so once unasked, then answers each probe for it.
	const std::string asked = labels(listing.host).substr(0, 1 + listing.host.size());
	std::string elsewhere(4, '\0');
	const std::uint32_t other = address("192.0.2.1");
	std::memcpy(elsewhere.data(), &other, 4);
	const std::string answer = word(0) + word(0x8400) + word(0) + word(1) + word(0) + word(0) +
	                           record(listing.host + ".local.", MDNS_RECORDTYPE_A, elsewhere, true);
	while (responder.read()) {
	} // what it heard of the first round
	responder.send(answer);
	bool probedAgain = false;
	REQUIRE(turnUntil(
		{&served}, [&] { return served.announced.size() == 2; }, responder.fd(),
		[&] {
			while (auto datagram = responder.read()) {
				if (datagram->response || datagram->bytes.find(asked) == std::string::npos) continue;
				probedAgain = probedAgain || datagram->authorities > 0;
				responder.send(answer);
			}
		}));
	// Section 9: probed again under the name it held, and renamed only once that probe lost.
	CHECK(probedAgain);
	Listing renamed = listing;
	renamed.host = "ear-test-late-clash-2";
	CHECK(served.announced[1] == renamed);
}

TEST_CASE("Past 15 clashes in 10 s, the next round of probing waits 5 s first", "[mdns][advertiser][probing]") {
	Listing listing = listingFor("_ear-paced._tcp");
	listing.host = "ear-test-paced";
	MdnsSocket responder(true);
	Served served(listing);
	std::string elsewhere(4, '\0');
	const std::uint32_t other = address("192.0.2.1");
	std::memcpy(elsewhere.data(), &other, 4);
	// The first probe of each host name it tries, and when: each is answered by a holder of that name, so each is a
	// clash, and the 16th clash in a row (the probe of "-16" answered) makes "-17" wait.
	std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> firstProbes;
	REQUIRE(turnUntil(
		{&served}, [&] { return firstProbes.size() == 17; }, responder.fd(),
		[&] {
			while (auto datagram = responder.read()) {
				if (datagram->response || datagram->authorities == 0) continue;
				const auto host = std::ranges::find_if(datagram->records, [&](const Record& each) {
					return each.section == MDNS_ENTRYTYPE_AUTHORITY && each.type == MDNS_RECORDTYPE_A &&
				           each.name.starts_with(listing.host);
				});
				if (host == datagram->records.end()) continue;
				if (firstProbes.empty() || firstProbes.back().first != host->name)
					firstProbes.emplace_back(host->name, std::chrono::steady_clock::now());
				responder.send(word(0) + word(0x8400) + word(0) + word(1) + word(0) + word(0) +
			                   record(host->name, MDNS_RECORDTYPE_A, elsewhere, true));
			}
		},
		std::chrono::seconds(30)));
	CHECK(firstProbes[15].first == listing.host + "-16.local.");
	CHECK(firstProbes[16].first == listing.host + "-17.local.");
	// Before the pace: each round's wait at most 250 ms. After it: 5 s, which no quick round could reach.
	CHECK(firstProbes[15].second - firstProbes[14].second < std::chrono::seconds(5));
	CHECK(firstProbes[16].second - firstProbes[15].second >= std::chrono::seconds(5));
}

TEST_CASE("A responder answering for its host with another address renames the host to '<host>-2'",
          "[mdns][advertiser][probing]") {
	Listing listing = listingFor("_ear-host-clash._tcp");
	listing.host = "ear-test-clash-host";
	MdnsSocket responder(true);
	Served served(listing);
	// Its first label: the probe may point at the ".local" it wrote before.
	const std::string asked = labels(listing.host).substr(0, 1 + listing.host.size());
	std::string elsewhere(4, '\0');
	const std::uint32_t other = address("192.0.2.1");
	std::memcpy(elsewhere.data(), &other, 4);
	const std::string answer = word(0) + word(0x8400) + word(0) + word(1) + word(0) + word(0) +
	                           record(listing.host + ".local.", MDNS_RECORDTYPE_A, elsewhere, true);
	REQUIRE(turnUntil(
		{&served}, [&] { return !served.announced.empty(); }, responder.fd(),
		[&] {
			while (auto datagram = responder.read())
				if (!datagram->response && datagram->bytes.find(asked) != std::string::npos) responder.send(answer);
		}));
	Listing renamed = listing;
	renamed.host = "ear-test-clash-host-2";
	CHECK(served.announced[0] == renamed);
}

TEST_CASE("Two advertisers sharing a host and its address with different instances both keep their names",
          "[mdns][advertiser][probing]") {
	Listing a = listingFor("_ear-shared-host._tcp");
	a.instance = "ear-test-a";
	Listing b = a;
	b.instance = "ear-test-b";
	b.port = 4322;
	Served first(a);
	Served second(b);
	REQUIRE(turnUntil({&first, &second}, [&] { return !first.announced.empty() && !second.announced.empty(); }));
	CHECK(first.announced[0] == a);
	CHECK(second.announced[0] == b);
}

TEST_CASE("An advertiser destroyed before it announced says no goodbye", "[mdns][advertiser][probing]") {
	const Listing listing = listingFor("_ear-no-goodbye._tcp");
	MdnsSocket browser(true);
	auto served = std::make_unique<Served>(listing);
	bool probed = false;
	REQUIRE(turnUntil(
		{served.get()}, [&] { return probed; }, browser.fd(),
		[&] {
			while (auto datagram = browser.read()) probed = probed || isProbeFor(*datagram, qualified(listing));
		}));
	served.reset();
	// Whatever it sent as it went came before this marker, which the browser hears back.
	constexpr std::uint16_t id = 0x6EB7;
	browser.send(marker(id));
	bool marked = false;
	std::size_t heardOfIt = 0;
	REQUIRE(turnUntil(
		{}, [&] { return marked; }, browser.fd(),
		[&] {
			while (auto datagram = browser.read()) {
				if (!datagram->response && datagram->id == id) marked = true;
				else if (datagram->response && datagram->bytes.find(listing.service.substr(1, 10)) != std::string::npos)
					++heardOfIt;
			}
		}));
	CHECK(heardOfIt == 0);
}
