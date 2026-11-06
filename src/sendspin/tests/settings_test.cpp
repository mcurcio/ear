// A sendspin node's settings: each refusal's exact text, in order, and the URL's grammar (docs/design/sendspin.md
// section 4; this slice's grammar: section 14, S1, and criterion 5).
#include <string>

#include "catch2/catch.hpp"
#include "ear/sendspin/sendspin_type.hpp"
#include "settings.hpp"

using ear::NodeSettings;
using ear::sendspin::parseSettings;
using ear::sendspin::ServerUrl;

namespace {

constexpr auto url = "ws://10.0.0.1:8927/sendspin";

std::string refusal(const NodeSettings& settings) {
	const auto parsed = parseSettings("ma", settings);
	REQUIRE_FALSE(parsed.has_value());
	return parsed.error();
}

} // namespace

TEST_CASE("Each refusal says what it refuses, the first failure in order", "[sendspin][settings]") {
	CHECK(refusal(NodeSettings::array()) == R"(settings: must be an object with optional "server", "port" and "name")");
	CHECK(refusal(5) == R"(settings: must be an object with optional "server", "port" and "name")");
	CHECK(refusal({{"bogus", 1}, {"server", 5}}) == "unknown key bogus");
	CHECK(refusal({{"port", 8928}, {"server", url}}) == "unknown key port");   // this slice only dials
	CHECK(refusal(NodeSettings::object()) == "server: missing");
	CHECK(refusal({{"name", ""}}) == "server: missing");
	CHECK(refusal({{"server", 5}}) == "server: must be ws://<host>[:<port>]/<path>");
	CHECK(refusal({{"server", "wss://a/b"}, {"name", ""}}) == "server: must be ws://<host>[:<port>]/<path>");
	CHECK(refusal({{"server", url}, {"name", ""}}) == "name: must be 1 to 64 bytes of UTF-8 with no control characters");
	CHECK(refusal({{"server", url}, {"name", 5}}) == "name: must be 1 to 64 bytes of UTF-8 with no control characters");
}

TEST_CASE("ServerUrl takes ws:// with a host, an optional port and a path", "[sendspin][settings]") {
	for (const char* good : {"ws://10.0.0.1:8927/sendspin", "ws://ma.local/sendspin", "ws://a/b", "ws://a-b.c:1/d/e_f.g~h-i",
	                         "ws://a:65535/b"}) {
		INFO(good);
		const auto parsed = ServerUrl::parse(good);
		REQUIRE(parsed.has_value());
		CHECK(parsed->text() == good);
	}
}

TEST_CASE("ServerUrl refuses anything else", "[sendspin][settings]") {
	for (const char* bad : {"wss://a/b", "http://a/b", "ws://a", "ws://u@a/b", "ws://u:p@a/b", "ws://a/b?c", "ws://a/b#c",
	                        "ws://[::1]/b", "ws://a:0/b", "ws://a:65536/b", "ws://a:/b", "ws://a:x/b", "ws:// a/b",
	                        "ws://a/b c", "ws:///b", "", "a/b"}) {
		INFO(bad);
		CHECK_FALSE(ServerUrl::parse(bad).has_value());
	}
	CHECK(ServerUrl::parse("ws://" + std::string(253, 'a') + "/b").has_value());
	CHECK_FALSE(ServerUrl::parse("ws://" + std::string(254, 'a') + "/b").has_value());
}

TEST_CASE("A name is 1 to 64 bytes of UTF-8 with no control characters; absent, the node's", "[sendspin][settings]") {
	const auto name = [](const std::string& text) { return NodeSettings{{"server", url}, {"name", text}}; };
	CHECK(parseSettings("ma", {{"server", url}})->name == "ma");
	CHECK(parseSettings("ma", name("Kitchen"))->name == "Kitchen");
	CHECK(parseSettings("ma", name(std::string(64, 'x')))->name.size() == 64);
	CHECK(parseSettings("ma", name("K\xC3\xBC" "che \xF0\x9F\x8E\xB5")).has_value());   // ü and a four-byte note

	CHECK_FALSE(parseSettings("ma", name(std::string(65, 'x'))).has_value());
	for (const char* bad : {"a\x01z", "a\x7fz", "a\nz", "\xff", "\xC3", "\xC0\x80", "\xE0\x80\x80", "\xED\xA0\x80",
	                        "\xF4\x90\x80\x80", "\xF0\x9F\x8E"}) {
		INFO(bad);
		CHECK_FALSE(parseSettings("ma", name(bad)).has_value());
	}
}

TEST_CASE("The type declares its ports and its settings' kinds, and refuses through InvalidSettings", "[sendspin][settings]") {
	const auto type = ear::sendspin::makeSendspinNodeType(nullptr);
	CHECK(type->name() == "sendspin");
	const auto kinds = type->settingKinds();
	REQUIRE(kinds.size() == 2);
	CHECK(kinds[0].key == "server");
	CHECK(kinds[0].kind == ear::SettingKind::text);
	CHECK(kinds[1].key == "name");
	CHECK(kinds[1].kind == ear::SettingKind::text);

	const auto refused = type->parse("ma", NodeSettings::object());
	REQUIRE_FALSE(refused.has_value());
	CHECK(refused.error().node == "ma");
	CHECK(refused.error().detail == "server: missing");

	const auto config = type->parse("ma", {{"server", url}});
	REQUIRE(config.has_value());
	const ear::Ports& ports = (*config)->ports();
	CHECK(ports.inputs == std::vector<ear::PortName>{"to_server_FL", "to_server_FR"});
	CHECK(ports.outputs == std::vector<ear::PortName>{"from_server_FL", "from_server_FR"});
	CHECK(ports.insideInputs.empty());
	CHECK(ports.insideOutputs.empty());
}
