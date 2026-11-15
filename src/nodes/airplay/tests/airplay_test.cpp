// The airplay type through TypeHarness (docs/node-types.md#airplay): its spec, ports and speaker names, the program and
// pipe receiver the framework makes of its configure, and its report loop fed a real captured feed. The loop runs a
// real Advertiser (ear_mdns), so a listing is probed and goes out on the dev container's network as ear's does: the
// test waits on the loop's real descriptors (poll), then rings them. Cases side by side list speakers of their own
// names, so only the case that means to clash does; whether a sender finds a listing, plays to it and sees it go is the
// system test's (test/system/airplay_test.cpp).
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "airplay_spec.hpp"
#include "captured_feed.hpp"
#include "catch2/catch.hpp"
#include "ear/nodes/airplay.hpp"
#include "ear/testing/type_harness.hpp"
#include "notes.hpp"
#include "reports.hpp"

namespace airplay_test {
namespace airplay = ear::nodes::airplay;
using Harness = ear::testing::TypeHarness<airplay::spec, airplay::Airplay>;

std::string failure(const Harness& h) {
	const ear::NodeHealth health = h.health();
	return health.reason ? std::string(ear::failureCode(health.reason->code)) + ": " + health.reason->detail : "";
}

/// Added and running: its block, its pipe receiver and its program ready, its loop's staged work run.
void running(Harness& h, const ear::NodeSettings& settings = ear::NodeSettings::object()) {
	REQUIRE_FALSE(h.parse(settings));
	h.add();
	h.blockReady();
	h.ready(airplay::fromAirplay);
	REQUIRE(h.programs().size() == 1);
	REQUIRE(h.phase() == "running");
	h.runLoops();
}

std::size_t saidCount(const Harness& h, std::string_view prefix) {
	const auto& notes = const_cast<Harness&>(h)->notes();
	return static_cast<std::size_t>(
		std::ranges::count_if(notes, [&](const std::string& n) { return n.starts_with(prefix); }));
}

/// `feed` up to and including its item number `items` (counted from 1).
std::string_view feedThrough(std::size_t items, std::string_view feed = capturedFeed) {
	std::size_t at = 0;
	for (std::size_t i = 0; i < items; ++i) at = feed.find("</item>\n", at) + 8;
	return feed.substr(0, at);
}

std::string base64(std::string_view bytes) {
	constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string text;
	for (std::size_t at = 0; at < bytes.size(); at += 3) {
		const std::size_t left = std::min<std::size_t>(3, bytes.size() - at);
		std::uint32_t group = 0;
		for (std::size_t i = 0; i < 3; ++i)
			group = (group << 8U) | (i < left ? static_cast<unsigned char>(bytes[at + i]) : 0U);
		for (std::size_t i = 0; i < 4; ++i) text += i <= left ? alphabet[(group >> (18 - 6 * i)) & 0x3FU] : '=';
	}
	return text;
}

/// The captured feed with its speaker named `speaker`: every listing's instance `D6065625AF29@<speaker>`, written back
/// as shairport-sync writes items.
std::string capturedFeedOf(std::string_view speaker) {
	airplay::ReportParser parser;
	std::string feed;
	for (airplay::Report report : parser.feed(capturedFeed)) {
		if (report.code == airplay::svrg) {
			const auto at = report.data.find("@Kitchen\n");
			REQUIRE(at != std::string::npos);
			report.data.replace(at + 1, 7, speaker);
		}
		std::array<char, 80> head{};
		std::snprintf(head.data(), head.size(), "<item><type>%x</type><code>%x</code><length>%zu</length>", report.type,
		              report.code, report.data.size());
		feed += head.data();
		if (!report.data.empty()) feed += "\n<data encoding=\"base64\">\n" + base64(report.data) + "</data>";
		feed += "</item>\n";
	}
	REQUIRE(parser.dropped() == 0);
	return feed;
}

/// Only fails a test: each wait ends at the event it waits for.
constexpr std::chrono::seconds deadline{10};

/// The report loop's watched descriptors waited on (poll) and rung, its work run, and `also` (a descriptor the test
/// serves, with its handler) served too, until `done()`; false at the deadline.
bool loopsUntil(Harness& h, const std::function<bool()>& done, int also = -1, const std::function<void()>& serve = {}) {
	const auto until = std::chrono::steady_clock::now() + deadline;
	for (;;) {
		if (done()) return true;
		const auto left =
			std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
		if (left <= 0) return false;
		std::vector<pollfd> ready;
		for (const int fd : h.watchedDescriptors(airplay::reports)) ready.push_back(pollfd{fd, POLLIN, 0});
		ready.push_back(pollfd{also, POLLIN, 0});
		if (::poll(ready.data(), ready.size(), static_cast<int>(left)) <= 0) continue;
		for (std::size_t i = 0; i + 1 < ready.size(); ++i)
			if ((ready[i].revents & POLLIN) != 0) h.readable(ready[i].fd);
		h.runLoops();
		if (also >= 0 && (ready.back().revents & POLLIN) != 0) serve();
	}
}

/// A socket in the mDNS group on port 5353, as a browser's: it hears every listing's announcements and goodbyes.
class GroupListener {
public:
	GroupListener() : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {
		REQUIRE(fd_ >= 0);
		const int enable = 1;
		REQUIRE(::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable) == 0);
		REQUIRE(::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof enable) == 0);
		sockaddr_in any{};
		any.sin_family = AF_INET;
		any.sin_port = htons(5353);
		any.sin_addr.s_addr = htonl(INADDR_ANY);
		REQUIRE(::bind(fd_, reinterpret_cast<const sockaddr*>(&any), sizeof any) == 0);
		ip_mreq group{};
		group.imr_multiaddr.s_addr = group_;
		group.imr_interface.s_addr = htonl(INADDR_ANY);
		REQUIRE(::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof group) == 0);
	}
	~GroupListener() { ::close(fd_); }
	GroupListener(const GroupListener&) = delete;
	GroupListener& operator=(const GroupListener&) = delete;

	/// How many goodbyes (a _raop._tcp PTR with TTL 0) for an instance holding `instance` arrived before a marker query
	/// this sends now comes back: whatever went out before the call came before the marker.
	std::size_t goodbyesFor(std::string_view instance) {
		static constexpr char markerBytes[] =
			"\x7e\x57\0\0\0\x01\0\0\0\0\0\0\x07_marker\x04_tcp\x05local\0\0\x0c\0\x01";
		constexpr std::string_view marker(markerBytes, sizeof markerBytes - 1);
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_port = htons(5353);
		to.sin_addr.s_addr = group_;
		REQUIRE(::sendto(fd_, marker.data(), marker.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to) ==
		        static_cast<ssize_t>(marker.size()));
		static constexpr char pointerBytes[] =
			"\x05_raop\x04_tcp\x05local\0\0\x0c\0\x01"; // the PTR's name, type, class
		constexpr std::string_view pointer(pointerBytes, sizeof pointerBytes - 1);
		std::size_t goodbyes = 0;
		std::array<char, 9000> buffer{};
		for (const auto until = std::chrono::steady_clock::now() + deadline;;) {
			const auto left =
				std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
			REQUIRE(left > 0);
			pollfd ready{fd_, POLLIN, 0};
			if (::poll(&ready, 1, static_cast<int>(left)) <= 0) continue;
			const auto got = ::recv(fd_, buffer.data(), buffer.size(), 0);
			if (got < 12) continue;
			const std::string_view packet(buffer.data(), static_cast<std::size_t>(got));
			if (packet == marker) return goodbyes;
			const auto at = packet.find(pointer);
			if ((packet[2] & 0x80) == 0 || at == std::string_view::npos || at + pointer.size() + 4 > packet.size())
				continue;
			const bool ttlZero = packet.substr(at + pointer.size(), 4) == std::string_view("\0\0\0\0", 4);
			if (ttlZero && packet.find(instance, at) != std::string_view::npos) ++goodbyes;
		}
	}

private:
	int fd_;
	in_addr_t group_ = inet_addr("224.0.0.251");
};

/// shairport-sync's report item for a listing (an 'svrg' item): `data` base64-encoded as it writes it.
std::string svrgItem(std::string_view data) {
	return "<item><type>73736e63</type><code>73767267</code><length>" + std::to_string(data.size()) +
	       "</length>\n<data encoding=\"base64\">\n" + base64(data) + "</data></item>\n";
}

/// Until the node said `count` notes starting "listed ".
void untilListed(Harness& h, std::size_t count = 1) {
	REQUIRE(loopsUntil(h, [&] { return saidCount(h, "listed ") >= count; }));
}

} // namespace airplay_test

using namespace airplay_test;

TEST_CASE(
	"Airplay declares out.1 and out.2, an inside bank from_airplay, an optional text setting name and an optional "
	"boolean setting airplay2",
	"[airplay]") {
	Harness h;
	REQUIRE_FALSE(h.parse(ear::NodeSettings::object()));
	CHECK(h->ports().inputs().empty());
	CHECK(h->ports().outputs() == std::vector<ear::PortName>{"out.1", "out.2"});
	CHECK(ear::nodes::makeAirplayNodeType()->name() == "airplay"); // its setting's kind: test/nodes' declared kinds
}

TEST_CASE("A speaker name is 1 to 50 bytes of UTF-8 with no control character, no %, no @ and no dot",
          "[airplay][settings]") {
	using airplay::SpeakerName;
	CHECK(SpeakerName::parse("Kitchen")->text() == "Kitchen");
	CHECK(SpeakerName::parse("K\xC3\xBC"
	                         "che")
	          .has_value());
	CHECK(SpeakerName::parse(std::string(50, 'a')).has_value());
	CHECK_FALSE(SpeakerName::parse(std::string(51, 'a')));
	CHECK_FALSE(SpeakerName::parse(""));
	CHECK_FALSE(SpeakerName::parse("Kitchen\n"));
	CHECK_FALSE(SpeakerName::parse("Kit\x7F"));
	CHECK_FALSE(SpeakerName::parse("Kitchen %h"));
	CHECK_FALSE(SpeakerName::parse("me@Kitchen"));
	CHECK_FALSE(SpeakerName::parse("Mr. K"));
	CHECK_FALSE(SpeakerName::parse("\xFF"));
	CHECK_FALSE(SpeakerName::parse("\xC3"));
	Harness h;
	const std::optional<std::string> refused(
		"name: must be a speaker name: 1 to 50 bytes, no control characters, no %, @ or dot");
	CHECK(h.parse({{"name", "a@b"}}) == refused);
	CHECK(h.parse({{"name", "Mr. K"}}) == refused);
}

TEST_CASE("The host label is ear- and the node's name, each underscore a dash, cut to 63 bytes", "[airplay]") {
	CHECK(airplay::hostLabelOf("kitchen") == "ear-kitchen");
	CHECK(airplay::hostLabelOf("back_porch") == "ear-back-porch");
	CHECK(airplay::hostLabelOf(std::string(80, 'x')) == "ear-" + std::string(59, 'x'));
}

TEST_CASE("The program is shairport-sync with its config, the speaker name and the granted port, and the config names "
          "the FIFO, the feed, AirPlay 1 outright and its format, ignores the sender's volume and lets the kernel pick "
          "UDP ports",
          "[airplay][program]") {
	Harness h({.node = "kitchen"});
	running(h, {{"name", "Kitchen"}});
	const ear::ProgramLaunch& launch = h.programs().at(0).launch;
	CHECK(launch.executable == "/usr/local/bin/shairport-sync");
	CHECK(launch.argv == std::vector<std::string>{"shairport-sync", "-c", "/ear-runtime/kitchen.1.config", "--name",
	                                              "Kitchen", "--port", "49152"});
	REQUIRE(launch.files.size() == 1);
	CHECK(launch.files[0].first == "/ear-runtime/kitchen.1.config");
	CHECK(launch.files[0].second ==
	      "general = { mdns_backend = \"metadata\"; output_backend = \"pipe\"; service_type = \"classic\"; "
	      "ignore_volume_control = \"yes\"; udp_port_base = 0; };\n"
	      "metadata = { enabled = \"yes\"; pipe_name = \"/ear-runtime/kitchen.1.feed\"; };\n"
	      "pipe = { name = \"/ear-runtime/kitchen.1.from_airplay.fifo\"; output_format = \"S16_LE\"; "
	      "output_rate = 44100; };\n");
	CHECK(launch.feed == std::optional<std::string>("/ear-runtime/kitchen.1.feed"));
	CHECK(launch.needs.empty());
}

TEST_CASE("With airplay2 the config sets AirPlay 2 outright with the node's device id and nqptp channel and its "
          "format, and the program needs the channel",
          "[airplay][program][airplay2]") {
	Harness h({.node = "kitchen"});
	running(h, {{"name", "Kitchen"}, {"airplay2", true}});
	const ear::ProgramLaunch& launch = h.programs().at(0).launch;
	const std::string deviceId = airplay::DeviceId::of(airplay::thisHostName(), "kitchen").configText();
	REQUIRE(launch.files.size() == 1);
	CHECK(launch.files[0].second ==
	      "general = { mdns_backend = \"metadata\"; output_backend = \"pipe\"; service_type = \"airplay2\"; "
	      "ignore_volume_control = \"yes\"; udp_port_base = 0; airplay_device_id = " +
	          deviceId +
	          "; nqptp_shared_memory_interface_name = \"/ear-kitchen\"; };\n"
	          "metadata = { enabled = \"yes\"; pipe_name = \"/ear-runtime/kitchen.1.feed\"; };\n"
	          "pipe = { name = \"/ear-runtime/kitchen.1.from_airplay.fifo\"; output_format = \"S32_LE\"; "
	          "output_rate = 48000; };\n");
	CHECK(launch.needs == std::vector<std::string>{"/ear-kitchen"});
	CHECK(launch.argv == std::vector<std::string>{"shairport-sync", "-c", "/ear-runtime/kitchen.1.config", "--name",
	                                              "Kitchen", "--port", "49152"});
}

TEST_CASE("With airplay2 the pipe receiver reads s32le at 48 kHz", "[airplay][program][airplay2]") {
	Harness h({.node = "kitchen"});
	running(h, {{"airplay2", true}});
	const auto* shape = std::get_if<ear::PipeReceiverShape>(&h.companion(airplay::fromAirplay).shape);
	REQUIRE(shape != nullptr);
	CHECK(shape->format == ear::PipeFormat::s32le);
	CHECK(shape->rate == ear::SampleRate::fixed<48'000>());
}

TEST_CASE("A device id is locally administered and unicast, the same for the same names, different for another host "
          "or node, written as libconfig's 64-bit hex and as the RAOP prefix",
          "[airplay][airplay2]") {
	using airplay::DeviceId;
	const DeviceId id = DeviceId::of("hawking", "kitchen");
	const auto firstByte = static_cast<unsigned>((id.bits() >> 40U) & 0xFFU);
	CHECK((firstByte & 0x02U) == 0x02U); // locally administered
	CHECK((firstByte & 0x01U) == 0U);    // unicast
	CHECK(id.bits() >> 48U == 0U);
	CHECK(id == DeviceId::of("hawking", "kitchen"));
	SECTION("differs by host") { CHECK_FALSE(id == DeviceId::of("einstein", "kitchen")); }
	SECTION("differs by node") { CHECK_FALSE(id == DeviceId::of("hawking", "porch")); }
	// FNV-1a 64 of "hawking/kitchen", its low 48 bits: fixed by the hash's definition, so it holds across builds.
	CHECK(id.raopPrefix() == "CAD06FC9EB03");
	CHECK(id.configText() == "0xCAD06FC9EB03L");
	for (const DeviceId one : {id, DeviceId::of("a", "b"), DeviceId::of("", "x")}) {
		const std::string prefix = one.raopPrefix();
		CHECK(prefix.size() == 12);
		CHECK(std::ranges::all_of(prefix, [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }));
		CHECK(one.configText() == "0x" + prefix + "L");
	}
}

TEST_CASE("A node's nqptp channel is /ear-<node>, kept whole to 63 bytes and hashed past them", "[airplay][airplay2]") {
	CHECK(airplay::channelOf("kitchen").text() == "/ear-kitchen");
	const std::string kept(58, 'k');
	CHECK(airplay::channelOf(kept).text() == "/ear-" + kept);
	CHECK(airplay::channelOf(kept).text().size() == 63);
	const std::string longName(64, 'n');
	const std::string hashed = airplay::channelOf(longName).text();
	CHECK(hashed.size() == 63);
	CHECK(hashed == "/ear-" + std::string(41, 'n') + "-078b793dde92c425");
	// Two long names alike in all but their last byte, so in the 41 bytes kept and in any cut, still have channels of
	// their own.
	const std::string other = std::string(63, 'n') + "m";
	CHECK(airplay::channelOf(other).text() != hashed);
	CHECK(airplay::channelOf(other).text().starts_with("/ear-" + std::string(41, 'n') + "-"));
}

TEST_CASE("With no name setting the speaker goes by the node's name", "[airplay][program]") {
	Harness h({.node = "porch"});
	running(h);
	const auto& argv = h.programs().at(0).launch.argv;
	CHECK(std::ranges::find(argv, "--name")[1] == "porch");
}

TEST_CASE("The pipe receiver is <node>.pipe on from_airplay, reading s16le at 44.1 kHz from its configuration's FIFO",
          "[airplay][program]") {
	Harness h({.node = "kitchen"});
	running(h);
	const ear::testing::CompanionFake& pipe = h.companion(airplay::fromAirplay);
	CHECK(pipe.kind == ear::CompanionKind::pipeReceiver);
	CHECK(pipe.name == "kitchen.pipe");
	CHECK(pipe.fifo == "/ear-runtime/kitchen.1.from_airplay.fifo");
	const auto* shape = std::get_if<ear::PipeReceiverShape>(&pipe.shape);
	REQUIRE(shape != nullptr);
	CHECK(shape->format == ear::PipeFormat::s16le);
	CHECK(shape->rate == ear::SampleRate::fixed<44'100>());
}

TEST_CASE("The report loop announces the listing once probed, updates it on the same instance's next listing, and "
          "says playing and idle",
          "[airplay][reports]") {
	Harness h({.node = "kitchen"});
	running(h, {{"name", "Den"}});
	const std::string feed = capturedFeedOf("Den");
	h.feedProgram(0, std::string(feedThrough(2, feed))); // its name, then its listing
	h.runLoops();
	CHECK(h.armed(airplay::reports) == 1); // the advertiser's descriptor, watched while it probes
	CHECK(saidCount(h, "listed ") == 0);
	untilListed(h);
	CHECK(h->notes() == std::vector<std::string>{"listed D6065625AF29@Den on port 5100"});
	h.feedProgram(0, feed.substr(feedThrough(2, feed).size()));
	h.runLoops();
	// The second and third listings are the same instance's: updated, never announced anew, and nothing failed.
	CHECK(h->notes() == std::vector<std::string>{"listed D6065625AF29@Den on port 5100",
	                                             std::string(airplay::notes::playing),
	                                             std::string(airplay::notes::idle)});
	h.remove();
	CHECK(h.gone());
}

TEST_CASE("AirPlay 2's two listings, _raop._tcp and _airplay._tcp, each get their own advertiser and listed note",
          "[airplay][reports][airplay2]") {
	Harness h({.node = "attic"});
	running(h, {{"name", "Attic"}, {"airplay2", true}});
	h.feedProgram(0, svrgItem("service=_raop._tcp\nname=0A1B2C3D4E5F@Attic\nport=7000\ntxt=cn=0,1\ntxt=tp=UDP\n") +
	                     svrgItem("service=_airplay._tcp\nname=Attic\nport=7000\ntxt=deviceid=0a:1b:2c:3d:4e:5f\n"));
	h.runLoops();
	CHECK(h.watchedDescriptors(airplay::reports).size() == 2);
	untilListed(h, 2);
	std::vector<std::string> notes = h->notes();
	std::ranges::sort(notes);
	CHECK(notes == std::vector<std::string>{"listed 0A1B2C3D4E5F@Attic on port 7000", "listed Attic on port 7000"});
	// The same instances again: updates, never new listings.
	h.feedProgram(0, svrgItem("service=_airplay._tcp\nname=Attic\nport=7000\ntxt=deviceid=0a:1b:2c:3d:4e:5f\n"));
	h.runLoops();
	CHECK(h.watchedDescriptors(airplay::reports).size() == 2);
	CHECK(h->notes().size() == 2);
	h.remove();
	CHECK(h.gone());
}

TEST_CASE("A listing of another instance replaces the first, and a bad one is said as advertise-failed",
          "[airplay][reports]") {
	Harness h({.node = "kitchen"});
	running(h);
	h.feedProgram(0, std::string(feedThrough(2, capturedFeedOf("Hall"))));
	h.runLoops();
	untilListed(h);
	h.feedProgram(0,
	              "<item><type>73736e63</type><code>73767267</code><length>55</length>\n<data "
	              "encoding=\"base64\">\nc2VydmljZT1fcmFvcC5fdGNwCm5hbWU9QkJCQkBLaXRjaGVuCnBvcnQ9NTEwMAp0eHQ9YT0xCg=="
	              "</data></item>\n"); // service=_raop._tcp name=BBBB@Kitchen port=5100 txt=a=1
	h.feedProgram(0, "<item><type>73736e63</type><code>73767267</code><length>16</length>\n<data "
	                 "encoding=\"base64\">\nbmFtZT1hQEsKcG9ydD0xCg==</data></item>\n"); // name=a@K port=1
	h.runLoops();
	untilListed(h, 2);
	CHECK(h->notes() == std::vector<std::string>{"listed D6065625AF29@Hall on port 5100",
	                                             "advertise-failed the listing has no service line",
	                                             "listed BBBB@Kitchen on port 5100"});
	CHECK(h.armed(airplay::reports) == 1);
}

TEST_CASE("A speaker whose instance another responder holds is renamed '(2)', said before its listed, and the same "
          "instance's next listings update it without a note",
          "[airplay][reports]") {
	// The holder: the same instance on another port and host, announced before the node probes.
	auto opened = ear::mdns::Advertiser::open(ear::mdns::Listing{.service = "_raop._tcp",
	                                                             .instance = "D6065625AF29@Kitchen",
	                                                             .host = "ear-test-holder",
	                                                             .port = 5200,
	                                                             .txt = {"am=Holder"}});
	REQUIRE(opened);
	ear::mdns::Advertiser& holder = **opened;
	bool held = false;
	const auto serveHolder = [&] {
		if (holder.turn().announced) held = true;
	};
	Harness h({.node = "kitchen"});
	running(h, {{"name", "Kitchen"}});
	REQUIRE(loopsUntil(h, [&] { return held; }, holder.descriptor(), serveHolder));

	h.feedProgram(0, std::string(feedThrough(2)));
	h.runLoops();
	REQUIRE(loopsUntil(h, [&] { return saidCount(h, "listed ") >= 1; }, holder.descriptor(), serveHolder));
	const std::vector<std::string> said{"renamed D6065625AF29@Kitchen to D6065625AF29@Kitchen (2)",
	                                    "listed D6065625AF29@Kitchen (2) on port 5100"};
	CHECK(h->notes() == said);
	// The rest of the feed lists the instance shairport-sync asked for: updates, never a new listing, and never a
	// goodbye to the one it holds.
	GroupListener browser;
	h.feedProgram(0, std::string(capturedFeed.substr(feedThrough(2).size())));
	h.runLoops();
	CHECK(browser.goodbyesFor("D6065625AF29@Kitchen (2)") == 0);
	std::vector<std::string> then = said;
	then.emplace_back(airplay::notes::playing);
	then.emplace_back(airplay::notes::idle);
	CHECK(h->notes() == then);
	h.remove();
	CHECK(h.gone());
}

TEST_CASE("shairport-sync's end fails the node, start-failed with its last line before it rested and lost after",
          "[airplay][program]") {
	const ear::ProgramExit exit{ear::ProgramExit::How::exited, 1, "pipename needed"};
	SECTION("before it rested") {
		Harness h({.autoComplete = false, .holdPrograms = true});
		REQUIRE_FALSE(h.parse(ear::NodeSettings::object()));
		h.add();
		h.blockReady();
		h.ready(airplay::fromAirplay);
		h.programStarted();
		h.programExited(0, exit);
		CHECK(failure(h) == "start-failed: shairport-sync exited with status 1: pipename needed");
	}
	SECTION("running") {
		Harness h;
		running(h);
		h.programExited(0, exit);
		CHECK(failure(h) == "lost: shairport-sync exited with status 1: pipename needed");
	}
}

TEST_CASE("A stop keeps shairport-sync running, and a removal stops it", "[airplay][program]") {
	Harness h;
	running(h);
	REQUIRE(h.stop().has_value());
	CHECK(h.phase() == "stopped");
	CHECK_FALSE(h.programs().at(0).stopped);
	h.remove();
	CHECK(h.programs().at(0).stopped);
	CHECK(h.gone());
}
