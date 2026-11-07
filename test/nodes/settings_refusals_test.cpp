// Every node type's settings refusals, byte for byte and in order, through NodeType::parse: the texts a user reads and
// which of several problems is told first. It says nothing of how a type checks its settings.
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "all_types.hpp"
#include "catch2/catch.hpp"
#include "ear/core/node_catalogue.hpp"
#include "ear/nodes/card.hpp"
#include "ear/nodes/file.hpp"
#include "ear/nodes/rtp.hpp"
#include "ear/nodes/sendspin.hpp"
#include "ear/nodes/white.hpp"

namespace {

using ear::NodeSettings;

constexpr std::string_view accepted = "(accepted)";

const std::map<std::string, std::unique_ptr<const ear::NodeType>, std::less<>>& types() {
	static const auto made = [] {
		std::map<std::string, std::unique_ptr<const ear::NodeType>, std::less<>> all;
		all.emplace("white", ear::nodes::makeWhiteNodeType());
		all.emplace("file", ear::nodes::makeFileNodeType(ear::test::recordingsFolder()));
		all.emplace("card", ear::nodes::makeCardNodeType());
		all.emplace("rtp", ear::nodes::makeRtpNodeType());
		all.emplace("sendspin", ear::nodes::makeSendspinNodeType(ear::test::configFolder()));
		return all;
	}();
	return made;
}

/// What `type` says of the settings: the refusal's text, or `accepted`.
std::string answer(std::string_view type, const NodeSettings& settings) {
	const auto parsed = types().find(type)->second->parse("n", settings);
	return parsed ? std::string(accepted) : parsed.error().detail;
}

/// As answer, the settings written as JSON text: big numbers are exactly what a parser makes of them.
std::string says(std::string_view type, std::string_view json) {
	INFO(type << " " << json);
	return answer(type, NodeSettings::parse(json));
}

constexpr std::string_view cardText =
	"card: must be an ALSA card id, 1-15 characters of [A-Za-z0-9_-] (as in /proc/asound/cards)";
constexpr std::string_view playbackText = "playback: must be a whole number of channels, 0-64";
constexpr std::string_view captureText = "capture: must be a whole number of channels, 0-64";
constexpr std::string_view addressText =
	R"(address: must be "0.0.0.0" or an IPv4 multicast group (224.0.0.0-239.255.255.255), as a dotted quad)";
constexpr std::string_view portText = "port: must be a whole number, 1-65535";
constexpr std::string_view formatText = R"(format: must be "L16" or "L24")";
constexpr std::string_view channelsText = "channels: must be a whole number, 1-64";
constexpr std::string_view rateText = "rate: must be a whole number of Hz, 8000-192000";
constexpr std::string_view latencyText = "latencyMs: must be a whole number of milliseconds, 1-1000";
constexpr std::string_view serverText = "server: must be ws://<host>[:<port>]/<path>";
constexpr std::string_view acrossText = R"(port: only without "server")";
constexpr std::string_view nameText = "name: must be 1 to 64 bytes of UTF-8 with no control characters";
constexpr std::string_view pathText = "path: must be a non-empty string";
constexpr std::string_view relativeText = R"(path: must be relative to the recordings folder, without "..")";

/// Valid settings of each type but for one key's value, the value written as JSON text.
std::string rtpWith(std::string_view port, std::string_view format = R"("L24")", std::string_view channels = "2") {
	return std::string(R"({"address":"0.0.0.0","port":)") + std::string(port) + R"(,"format":)" + std::string(format) +
	       R"(,"channels":)" + std::string(channels) + "}";
}
std::string rtpWithExtra(std::string_view key, std::string_view value) {
	return std::string(R"({"address":"0.0.0.0","port":5004,"format":"L24","channels":2,")") + std::string(key) +
	       R"(":)" + std::string(value) + "}";
}
std::string cardWith(std::string_view key, std::string_view value) {
	return std::string(R"({"card":"Pro24",")") + std::string(key) + R"(":)" + std::string(value) + "}";
}
std::string with(std::string_view key, std::string_view value) {
	return std::string(R"({")") + std::string(key) + R"(":)" + std::string(value) + "}";
}

/// Numbers no whole-number key takes, in any range below: each kind that is not one, and each size a careless compare
/// lets through: wrapped negative above INT64_MAX, a float above UINT64_MAX, and 1 when cut to 32 bits.
const std::vector<std::string_view> notAWholeNumber{R"("5004")",
                                                    "5004.5",
                                                    "48000.0",
                                                    "true",
                                                    "null",
                                                    "[]",
                                                    "{}",
                                                    "-1",
                                                    "9223372036854775808",
                                                    "18446744073709551615",
                                                    "18446744073709551616",
                                                    "4294967297"};

} // namespace

TEST_CASE("White takes no settings: null and an empty object, nothing else", "[nodes][settings]") {
	CHECK(says("white", "null") == accepted);
	CHECK(says("white", "{}") == accepted);
	for (const std::string_view bad : {"true", "3", "1.5", R"("x")", "[]", "[1]"}) {
		CHECK(says("white", bad) == "settings: must be an object");
	}
	CHECK(says("white", R"({"gain":1})") == "unknown key gain (white takes no settings)");
	CHECK(says("white", R"({"zeta":1,"alpha":2})") == "unknown key alpha (white takes no settings)");
}

TEST_CASE("File's refusals, in order", "[nodes][settings]") {
	for (const std::string_view bad : {"null", "true", "3", R"("x")", "[]"}) {
		CHECK(says("file", bad) == R"(settings: must be an object with "path")");
	}
	CHECK(says("file", R"({"gain":1})") == "unknown key gain");
	CHECK(says("file", "{}") == "path: missing");
	CHECK(says("file", R"({"path":null})") == pathText);
	CHECK(says("file", R"({"path":7})") == pathText);
	CHECK(says("file", R"({"path":""})") == pathText);
	CHECK(says("file", R"({"path":"/a"})") == relativeText);
	CHECK(says("file", R"({"path":"a/../b"})") == relativeText);
	CHECK(says("file", R"({"path":"take1.wav"})") == accepted);
	// Several problems: the first in order is told.
	CHECK(says("file", R"({"path":7,"zeta":1})") == "unknown key zeta");
	CHECK(says("file", R"({"zeta":1,"alpha":2,"path":""})") == "unknown key alpha");
}

TEST_CASE("Card's refusals, in order", "[nodes][settings]") {
	for (const std::string_view bad : {"null", "true", "3", R"("x")", "[]"}) {
		CHECK(says("card", bad) == R"(settings: must be an object with "card")");
	}
	CHECK(says("card", R"({"gain":1})") == "unknown key gain");
	CHECK(says("card", "{}") == "card: missing");
	CHECK(says("card", R"({"playback":2})") == "card: missing");
	CHECK(says("card", R"({"card":null})") == cardText);
	CHECK(says("card", R"({"card":3})") == cardText);
	CHECK(says("card", R"({"card":"hw:0"})") == cardText);
	CHECK(says("card", cardWith("playback", "65")) == playbackText);
	CHECK(says("card", cardWith("capture", "65")) == captureText);
	CHECK(says("card", R"({"card":"Pro24"})") == "playback or capture: at least one must be above 0");
	CHECK(says("card", R"({"card":"Pro24","playback":0,"capture":0})") ==
	      "playback or capture: at least one must be above 0");
	CHECK(says("card", cardWith("playback", "64")) == accepted);
	CHECK(says("card", cardWith("capture", "1")) == accepted);
	for (const std::string_view bad : notAWholeNumber) {
		CHECK(says("card", cardWith("playback", bad)) == playbackText);
		CHECK(says("card", cardWith("capture", bad)) == captureText);
	}
	// Several problems: the first in order is told.
	CHECK(says("card", R"({"card":"hw:0","playback":65})") == cardText);
	CHECK(says("card", R"({"card":"Pro24","playback":65,"capture":-1})") == playbackText);
	CHECK(says("card", R"({"card":"hw:0","playback":65,"zeta":1})") == "unknown key zeta");
}

TEST_CASE("RTP's refusals, in order", "[nodes][settings]") {
	for (const std::string_view bad : {"null", "true", "3", R"("x")", "[]"}) {
		CHECK(says("rtp", bad) == R"(settings: must be an object with "address", "port", "format" and "channels")");
	}
	CHECK(says("rtp", R"({"gain":1})") == "unknown key gain");
	CHECK(says("rtp", "{}") == "address: missing");
	CHECK(says("rtp", R"({"address":null})") == addressText);
	CHECK(says("rtp", R"({"address":239})") == addressText);
	CHECK(says("rtp", R"({"address":"192.168.1.1"})") == addressText);
	CHECK(says("rtp", R"({"address":"0.0.0.0"})") == "port: missing");
	CHECK(says("rtp", R"({"address":"0.0.0.0","port":5004})") == "format: missing");
	CHECK(says("rtp", R"({"address":"0.0.0.0","port":5004,"format":"L24"})") == "channels: missing");
	CHECK(says("rtp", rtpWith("0")) == portText);
	CHECK(says("rtp", rtpWith("65536")) == portText);
	CHECK(says("rtp", rtpWith("5004", "null")) == formatText);
	CHECK(says("rtp", rtpWith("5004", "24")) == formatText);
	CHECK(says("rtp", rtpWith("5004", R"("l24")")) == formatText);
	CHECK(says("rtp", rtpWith("5004", R"("L24")", "0")) == channelsText);
	CHECK(says("rtp", rtpWith("5004", R"("L24")", "65")) == channelsText);
	CHECK(says("rtp", rtpWithExtra("rate", "7999")) == rateText);
	CHECK(says("rtp", rtpWithExtra("rate", "192001")) == rateText);
	CHECK(says("rtp", rtpWithExtra("latencyMs", "0")) == latencyText);
	CHECK(says("rtp", rtpWithExtra("latencyMs", "1001")) == latencyText);
	CHECK(says("rtp", rtpWith("5004")) == accepted);
	CHECK(says("rtp", rtpWith("1", R"("L16")", "64")) == accepted);
	CHECK(says("rtp", rtpWithExtra("rate", "8000")) == accepted);
	CHECK(says("rtp", rtpWithExtra("latencyMs", "1000")) == accepted);
	for (const std::string_view bad : notAWholeNumber) {
		CHECK(says("rtp", rtpWith(bad)) == portText);
		CHECK(says("rtp", rtpWith("5004", R"("L24")", bad)) == channelsText);
		CHECK(says("rtp", rtpWithExtra("rate", bad)) == rateText);
		CHECK(says("rtp", rtpWithExtra("latencyMs", bad)) == latencyText);
	}
	// Several problems: the first in order is told.
	CHECK(says("rtp", R"({"address":"1.2.3.4","port":0})") == addressText);
	CHECK(says("rtp", R"({"address":"0.0.0.0","port":null})") == portText);
	CHECK(says("rtp", R"({"address":"0.0.0.0","port":0,"format":"x","channels":0})") == portText);
	CHECK(says("rtp", rtpWith("5004", R"("x")", "0")) == formatText);
	CHECK(says("rtp", R"({"address":"1.2.3.4","port":0,"zeta":1})") == "unknown key zeta");
}

TEST_CASE("Sendspin's refusals, in order", "[nodes][settings]") {
	constexpr std::string_view url = "ws://10.0.0.1:8927/sendspin";
	const auto serverAnd = [&](std::string_view rest) {
		return std::string(R"({"server":")") + std::string(url) + R"(",)" + std::string(rest) + "}";
	};
	for (const std::string_view bad : {"null", "true", "3", R"("x")", "[]"}) {
		CHECK(says("sendspin", bad) == R"(settings: must be an object with optional "server", "port" and "name")");
	}
	CHECK(says("sendspin", R"({"gain":1})") == "unknown key gain");
	CHECK(says("sendspin", R"({"server":5})") == serverText);
	CHECK(says("sendspin", R"({"server":null})") == serverText);
	CHECK(says("sendspin", R"({"name":5})") == nameText);
	CHECK(says("sendspin", R"({"name":null})") == nameText);
	CHECK(says("sendspin", R"({"port":0})") == portText);
	CHECK(says("sendspin", R"({"port":65536})") == portText);
	CHECK(says("sendspin", "{}") == accepted);
	CHECK(says("sendspin", R"({"port":65535})") == accepted);
	CHECK(says("sendspin", serverAnd(R"("name":"Kitchen")")) == accepted);
	for (const std::string_view bad : notAWholeNumber) {
		CHECK(says("sendspin", with("port", bad)) == portText);
	}
	// Several problems: the first in order is told.
	CHECK(says("sendspin", R"({"server":"wss://a/b","port":0})") == serverText);
	CHECK(says("sendspin", serverAnd(R"("port":8928)")) == acrossText);
	CHECK(says("sendspin", serverAnd(R"("port":"x","name":"")")) == acrossText);
	CHECK(says("sendspin", serverAnd(R"("port":null)")) == acrossText);
	CHECK(says("sendspin", R"({"server":5,"port":8928})") == serverText);
	CHECK(says("sendspin", R"({"port":0,"name":""})") == portText);
	CHECK(says("sendspin", serverAnd(R"("name":"")")) == nameText);
	CHECK(says("sendspin", R"({"server":5,"port":0,"name":"","zeta":1})") == "unknown key zeta");
}

TEST_CASE("Unknown keys are told in the object's sorted order, not as written", "[nodes][settings]") {
	for (const std::string_view type : {"file", "card", "rtp", "sendspin"}) {
		CHECK(says(type, R"({"zeta":1,"alpha":2})") == "unknown key alpha");
	}
}

TEST_CASE("Each type's setting kinds are what its parse checks", "[nodes][settings]") {
	// Settings each type accepts, and the same with one key at a time set wrongly for its declared kind: a key parse did
	// not know would be told as unknown, and a kind it did not check would pass.
	const std::map<std::string, NodeSettings> valid{{"white", NodeSettings::object()},
	                                                {"file", {{"path", "a.wav"}}},
	                                                {"card", {{"card", "Pro24"}, {"playback", 2}, {"capture", 2}}},
	                                                {"rtp",
	                                                 {{"address", "0.0.0.0"},
	                                                  {"port", 5004},
	                                                  {"format", "L24"},
	                                                  {"channels", 2},
	                                                  {"rate", 48000},
	                                                  {"latencyMs", 100}}},
	                                                {"sendspin", {{"port", 8928}, {"name", "Kitchen"}}}};
	for (const auto& [name, type] : types()) {
		INFO(name);
		const NodeSettings& good = valid.at(name);
		CHECK(answer(name, good) == accepted);
		CHECK(type->settingKinds().data() == type->settingKinds().data());
		for (const auto& [key, kind] : type->settingKinds()) {
			const std::string spelled(key);
			INFO(spelled);
			NodeSettings bad = good;
			bad[spelled] = kind == ear::SettingKind::text ? NodeSettings(5) : NodeSettings("5");
			const std::string told = answer(name, bad);
			CHECK(told.starts_with(spelled + ": must be "));
			if (kind == ear::SettingKind::wholeNumber) {
				bad[spelled] = 5.5;
				CHECK(answer(name, bad).starts_with(spelled + ": must be a whole number"));
			}
			// A key left out is missing or optional, never unknown.
			NodeSettings without = good;
			without.erase(spelled);
			CHECK_FALSE(answer(name, without).starts_with("unknown key"));
		}
		// Nothing but the declared keys is taken.
		NodeSettings extra = good;
		extra["undeclared"] = 1;
		CHECK(answer(name, extra).starts_with("unknown key undeclared"));
	}
}
