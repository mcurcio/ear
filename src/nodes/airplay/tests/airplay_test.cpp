// The airplay type through TypeHarness (docs/node-types.md#airplay): its spec, ports and speaker names, the program and
// pipe receiver the framework makes of its configure, and its report loop fed a real captured feed. The loop runs a
// real Advertiser (ear_mdns), so a listing goes out on the dev container's network as ear's does; whether a sender
// finds it, plays to it and sees it go is the system test's (test/system/airplay_test.cpp).
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

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

/// The captured feed up to and including its item number `items` (counted from 1).
std::string_view feedThrough(std::size_t items) {
	std::size_t at = 0;
	for (std::size_t i = 0; i < items; ++i) at = capturedFeed.find("</item>\n", at) + 8;
	return capturedFeed.substr(0, at);
}

} // namespace airplay_test

using namespace airplay_test;

TEST_CASE("Airplay declares out.1 and out.2, an inside bank from_airplay, and one optional text setting name",
          "[airplay]") {
	Harness h;
	REQUIRE_FALSE(h.parse(ear::NodeSettings::object()));
	CHECK(h->ports().inputs().empty());
	CHECK(h->ports().outputs() == std::vector<ear::PortName>{"out.1", "out.2"});
	CHECK(ear::nodes::makeAirplayNodeType()->name() == "airplay"); // its setting's kind: test/nodes' declared kinds
}

TEST_CASE("A speaker name is 1 to 50 bytes of UTF-8 with no control character, no % and no @", "[airplay][settings]") {
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
	CHECK_FALSE(SpeakerName::parse("\xFF"));
	CHECK_FALSE(SpeakerName::parse("\xC3"));
	Harness h;
	CHECK(h.parse({{"name", "a@b"}}) ==
	      std::optional<std::string>("name: must be a speaker name: 1 to 50 bytes, no control characters, no % or @"));
}

TEST_CASE("The host label is ear- and the node's name, each underscore a dash, cut to 63 bytes", "[airplay]") {
	CHECK(airplay::hostLabelOf("kitchen") == "ear-kitchen");
	CHECK(airplay::hostLabelOf("back_porch") == "ear-back-porch");
	CHECK(airplay::hostLabelOf(std::string(80, 'x')) == "ear-" + std::string(59, 'x'));
}

TEST_CASE("The program is shairport-sync with its config, the speaker name and the granted port, and the config names "
          "the FIFO, the feed and AirPlay 1's format",
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
	      "general = { mdns_backend = \"metadata\"; output_backend = \"pipe\"; };\n"
	      "metadata = { enabled = \"yes\"; pipe_name = \"/ear-runtime/kitchen.1.feed\"; };\n"
	      "pipe = { name = \"/ear-runtime/kitchen.1.from_airplay.fifo\"; output_format = \"S16_LE\"; "
	      "output_rate = 44100; };\n");
	CHECK(launch.feed == std::optional<std::string>("/ear-runtime/kitchen.1.feed"));
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

TEST_CASE("The report loop announces the listing once, updates it on the same instance's next listing, and says "
          "playing and idle",
          "[airplay][reports]") {
	Harness h({.node = "kitchen"});
	running(h, {{"name", "Kitchen"}});
	h.feedProgram(0, std::string(feedThrough(2))); // its name, then its listing
	h.runLoops();
	CHECK(h->notes() == std::vector<std::string>{"listed D6065625AF29@Kitchen on port 5100"});
	CHECK(h.armed(airplay::reports) == 1); // the advertiser's socket, watched
	h.feedProgram(0, std::string(capturedFeed.substr(feedThrough(2).size())));
	h.runLoops();
	// The second and third listings are the same instance's: updated, never announced anew, and nothing failed.
	CHECK(h->notes() == std::vector<std::string>{"listed D6065625AF29@Kitchen on port 5100",
	                                             std::string(airplay::notes::playing),
	                                             std::string(airplay::notes::idle)});
	h.remove();
	CHECK(h.gone());
}

TEST_CASE("A listing of another instance replaces the first, and a bad one is said as advertise-failed",
          "[airplay][reports]") {
	Harness h({.node = "kitchen"});
	running(h);
	h.feedProgram(0, std::string(feedThrough(2)));
	h.feedProgram(0,
	              "<item><type>73736e63</type><code>73767267</code><length>55</length>\n<data "
	              "encoding=\"base64\">\nc2VydmljZT1fcmFvcC5fdGNwCm5hbWU9QkJCQkBLaXRjaGVuCnBvcnQ9NTEwMAp0eHQ9YT0xCg=="
	              "</data></item>\n"); // service=_raop._tcp name=BBBB@Kitchen port=5100 txt=a=1
	h.feedProgram(0, "<item><type>73736e63</type><code>73767267</code><length>16</length>\n<data "
	                 "encoding=\"base64\">\nbmFtZT1hQEsKcG9ydD0xCg==</data></item>\n"); // name=a@K port=1
	h.runLoops();
	CHECK(h->notes() == std::vector<std::string>{"listed D6065625AF29@Kitchen on port 5100",
	                                             "listed BBBB@Kitchen on port 5100",
	                                             "advertise-failed the listing has no service line"});
	CHECK(h.armed(airplay::reports) == 1);
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
