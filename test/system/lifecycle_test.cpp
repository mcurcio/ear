// Slice 3a's closing system tests (docs/design/node-lifecycle.md section 12's L-8 and 11.7; control-plane.md L1's
// list): the lifecycle through the real out/ear, across restarts of ear, on a shared `disabled` value, on the test
// sound card and on a held UDP port. The stop and start and the disable and enable within one run of ear are in
// control_port_test.cpp. Every wait is for an event (a line ear prints, a message, ear's exit); a deadline only fails a
// test.
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ear_system.hpp"

namespace {

using namespace ear::test;

/// `text` with its one `from` replaced by `to`.
std::string replacedOnce(std::string text, const std::string& from, const std::string& to) {
	const std::size_t at = text.find(from);
	REQUIRE(at != std::string::npos);
	REQUIRE(text.find(from, at + 1) == std::string::npos);
	return text.replace(at, from.size(), to);
}

/// The test ALSA configuration (slice E2): card "Test" is a file PCM writing into EAR_ALSA_OUT, here `directory`.
void useTestCard(const TempDirectory& directory) {
	const auto alsa = std::filesystem::path(__FILE__).parent_path() / "../../src/pipewire/tests/alsa/alsa.conf";
	REQUIRE(std::filesystem::exists(alsa));
	::setenv("ALSA_CONFIG_PATH", alsa.c_str(), 1);
	::setenv("EAR_ALSA_OUT", directory.path().c_str(), 1);
}

/// A UDP socket bound to 0.0.0.0 at a free port without SO_REUSEADDR, held while it lives: PipeWire's rtp-source binds
/// the same port with SO_REUSEADDR and fails.
class HeldUdpPort {
public:
	HeldUdpPort() : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {
		if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "socket");
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0)
			throw std::system_error(errno, std::generic_category(), "bind");
		socklen_t length = sizeof address;
		if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
			throw std::system_error(errno, std::generic_category(), "getsockname");
		port_ = ntohs(address.sin_port);
	}
	~HeldUdpPort() { ::close(fd_); }
	HeldUdpPort(const HeldUdpPort&) = delete;
	HeldUdpPort& operator=(const HeldUdpPort&) = delete;
	std::uint16_t port() const noexcept { return port_; }

private:
	int fd_;
	std::uint16_t port_ = 0;
};

/// The first node `name` in a graph read, or null.
json nodeNamed(const json& graph, const std::string& name) {
	for (const auto& node : graph["nodes"])
		if (node["name"] == name) return node;
	return nullptr;
}

/// Whether `out` has `line` as a whole line before position `before`.
bool lineBefore(const std::string& out, const std::string& line, std::size_t before) {
	const std::size_t at = ("\n" + out).find("\n" + line + "\n");
	return at != std::string::npos && at < before;
}

} // namespace

TEST_CASE("node.stop writes nothing to ear.conf, and a restart of ear runs the node again", "[system][lifecycle]") {
	TempDirectory directory;
	writeGraphFile(directory, noiseToRec());
	const std::string before = readText(directory.graphFile());
	{
		Ear ear(directory, {linkedLine("noise:noise.mono", "rec:record.left")});
		Rpc rpc(ear.port());
		REQUIRE(rpc.call("graph.subscribe").contains("result"));
		CHECK(rpc.call("node.stop", {{"name", "noise"}})["result"] == json{{"applied", "stop"}});
		rpc.waitFor(
			[](const json& m) { return Rpc::matches(m, "node.state", {{"name", "noise"}, {"phase", "stopped"}}); });
		CHECK(ear.stop(rpc) == 0);
		INFO("ear printed:\n" << ear.process().output());
		CHECK(ear.process().hasLine("node noise stopped"));
		CHECK(readText(directory.graphFile()) == before);
	}
	// A stop is the running ear's alone: the next ear runs the node from the same file.
	Ear again(directory, {"node noise running"});
	INFO("ear printed:\n" << again.process().output());
	Rpc rpc(again.port());
	const json graph = rpc.call("graph.get")["result"];
	INFO(graph.dump());
	CHECK(nodeNamed(graph, "noise")["phase"] == "running");
	CHECK(nodeNamed(graph, "noise")["wanted"] == "running");
	CHECK(again.stop(rpc) == 0);
	CHECK(readText(directory.graphFile()) == before);
}

TEST_CASE("node.disable survives a restart of ear, disabled at boot with no starting before it, and node.enable gives "
          "the line back",
          "[system][lifecycle]") {
	TempDirectory directory;
	writeGraphFile(directory, noiseToRec());
	const std::string before = readText(directory.graphFile());
	const std::string disabledText = replacedOnce(before, "node white noise;", "node white noise { disabled true; }");
	{
		Ear ear(directory, {linkedLine("noise:noise.mono", "rec:record.left")});
		Rpc rpc(ear.port());
		CHECK(rpc.call("node.disable", {{"name", "noise"}})["result"] == json{{"applied", "disable"}});
		REQUIRE(ear.process().waitForLineStartingWith("graph-file written ", deadline));
		CHECK(readText(directory.graphFile()) == disabledText);
		CHECK(ear.stop(rpc) == 0);
		CHECK(readText(directory.graphFile()) == disabledText);
	}
	Ear again(directory, {"node noise disabled"});
	const std::string& out = again.process().output();
	INFO("ear printed:\n" << out);
	// Never configured: no starting record (nor any other state) for it before it is disabled.
	const std::size_t disabledAt = ("\n" + out).find("\nnode noise disabled\n");
	CHECK_FALSE(lineBefore(out, "node noise starting", disabledAt));
	CHECK(out.find("node noise ") == disabledAt);
	Rpc rpc(again.port());
	const json snapshot = rpc.call("graph.subscribe")["result"];
	INFO(snapshot.dump());
	const json noise = nodeNamed(snapshot, "noise");
	CHECK(noise["disabled"] == true);
	CHECK(noise["phase"] == "disabled");
	CHECK(noise["wanted"] == "disabled");

	CHECK(rpc.call("node.enable", {{"name", "noise"}})["result"] == json{{"applied", "enable"}});
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.state", {{"name", "noise"}, {"state", "running"}}); });
	REQUIRE(again.process().waitForLineStartingWith("graph-file written ", deadline));
	CHECK(readText(directory.graphFile()) == before);
	CHECK(again.stop(rpc) == 0);
	CHECK(readText(directory.graphFile()) == before);
	CHECK(again.process().hasLine("node noise starting: enabling"));
}

TEST_CASE("node.disable of one of two nodes sharing disabled $quiet replaces only its line, and the other plays on",
          "[system][lifecycle]") {
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("@set $quiet false;\n"
	                                        "node white a { disabled $quiet; }\n"
	                                        "node white b { disabled $quiet; }\n"
	                                        "node file rec { path \"rec.wav\"; }\n"
	                                        "route a:noise.mono rec:record.left;\n"
	                                        "route b:noise.mono rec:record.right;\n"));
	const std::string before = readText(directory.graphFile());
	Ear ear(directory, {linkedLine("b:noise.mono", "rec:record.right")});
	REQUIRE(ear.process().waitForLine(linkedLine("a:noise.mono", "rec:record.left"), deadline));
	Rpc rpc(ear.port());
	REQUIRE(rpc.call("graph.subscribe").contains("result"));

	const json disable = rpc.call("node.disable", {{"name", "a"}});
	INFO(disable.dump());
	CHECK(disable["result"] == json{{"applied", "disable"}});
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.state", {{"name", "a"}, {"phase", "disabled"}}); });
	const std::size_t disabledAt = rpc.indexOf("node.state", {{"name", "a"}, {"phase", "disabled"}});
	REQUIRE(ear.process().waitForLineStartingWith("graph-file written ", deadline));
	// Only a's own line: $quiet's @set and b's line are as they were (the owner's R34, "Only this node").
	CHECK(readText(directory.graphFile()) ==
	      replacedOnce(before, "node white a { disabled $quiet; }", "node white a { disabled true; }"));

	// b plays on: once a is disabled, a whole level window carries b's noise on record.right and nothing on
	// record.left (an input's meter stays live while its source is silent). The levels client goes before ear stops,
	// which waits for every connected client's close reply.
	{
		Rpc levels(ear.port());
		REQUIRE(levels.call("levels.subscribe").contains("result"));
		levels.waitFor([](const json& m) {
			if (m.value("method", "") != "levels" || !m["params"]["nodes"].contains("rec")) return false;
			const json& rec = m["params"]["nodes"]["rec"];
			return rec["record.left"][0].get<double>() == 0.0 && rec["record.right"][1].get<double>() > 0.5;
		});
	}

	INFO("notifications: " << json(rpc.notifications).dump());
	CHECK(rpc.indexOf("node.updated", {{"name", "b"}}) == rpc.notifications.size());
	CHECK(rpc.indexOf("node.state", {{"name", "b"}}) == rpc.notifications.size());
	const json graph = rpc.call("graph.get")["result"];
	INFO(graph.dump());
	CHECK(nodeNamed(graph, "b")["phase"] == "running");
	CHECK_FALSE(nodeNamed(graph, "b").contains("disabled"));
	CHECK(nodeNamed(graph, "a")["disabled"] == true);
	CHECK(disabledAt < rpc.notifications.size());
	CHECK(ear.stop(rpc) == 0);
	const std::string& out = ear.process().output();
	INFO(out);
	// b stopped once, when ear did, after a was disabled.
	const std::size_t bStopping = out.find("node b stopping");
	REQUIRE(bStopping != std::string::npos);
	CHECK(bStopping > out.find("node a disabled\n"));
	CHECK(out.find("node b stopping", bStopping + 1) == std::string::npos);
}

TEST_CASE("node.restart of a card node on the test card brings it back without a change to ear.conf",
          "[system][lifecycle][card]") {
	TempDirectory directory;
	useTestCard(directory);
	writeGraphFile(directory,
	               noiseToRec("record.left",
	                          "node card out { card \"Test\"; playback 2; }\nroute noise:noise.mono out:play.1;\n"));
	const std::string before = readText(directory.graphFile());
	Ear ear(directory, {linkedLine("noise:noise.mono", "out:play.1")});
	REQUIRE(ear.process().waitForLine("node out running", deadline));
	Rpc rpc(ear.port());
	REQUIRE(rpc.call("graph.subscribe").contains("result"));

	const json restart = rpc.call("node.restart", {{"name", "out"}});
	INFO(restart.dump());
	CHECK(restart["result"] == json{{"applied", "restart"}});
	const std::size_t replied = rpc.notifications.size();
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.state", {{"name", "out"}, {"state", "running"}}); });
	INFO("notifications: " << json(rpc.notifications).dump());
	const std::size_t stopping =
		rpc.indexOf("node.state", {{"name", "out"},
	                               {"phase", "stopping"},
	                               {"cause", {{"kind", "restarting"}, {"restart", {{"kind", "action"}}}}}});
	CHECK(stopping < replied); // the stopping the restart caused came before its reply
	CHECK(rpc.indexOf("node.state", {{"name", "out"}, {"phase", "configuring"}}, stopping) < rpc.notifications.size());
	CHECK(rpc.indexOf("graphFile.changed", json::object()) == rpc.notifications.size());
	CHECK(rpc.indexOf("node.updated", json::object()) == rpc.notifications.size());

	const json graph = rpc.call("graph.get")["result"];
	INFO(graph.dump());
	CHECK(nodeNamed(graph, "out")["phase"] == "running");
	bool linked = false;
	for (const auto& r : graph["routes"])
		if (r["to"] == json{{"node", "out"}, {"port", "play.1"}}) linked = r["status"] == "linked";
	CHECK(linked); // the block and its routes stayed through the restart

	CHECK(ear.stop(rpc) == 0);
	CHECK(readText(directory.graphFile()) == before);
	const std::string& out = ear.process().output();
	INFO(out);
	const std::size_t invoked = out.find("node out restart invoked\n");
	REQUIRE(invoked != std::string::npos);
	const std::size_t stoppingLine = out.find("node out stopping: restarting (restart invoked)\n");
	REQUIRE(stoppingLine != std::string::npos);
	CHECK(invoked < stoppingLine);
	CHECK(out.find("node out running\n", stoppingLine) != std::string::npos);
}

TEST_CASE("An rtp node on a UDP port another socket holds writes a still record, and graph.get shows waitingFor",
          "[system][lifecycle][rtp]") {
	TempDirectory directory;
	const HeldUdpPort held;
	writeGraphFile(directory, graphFileText("node rtp fpp { address \"0.0.0.0\"; port " + std::to_string(held.port()) +
	                                        "; format \"L24\"; channels 2; }\n"));
	// The node configures for good: PipeWire only logs the failed bind, and its receiver never runs.
	Ear ear(directory, {"node fpp still configuring after ", true});
	const std::string& out = ear.process().output();
	INFO("ear printed:\n" << out);
	const auto still = ear.process().lineStartingWith("node fpp still configuring after ");
	REQUIRE(still);
	CHECK(still->ends_with("s: waiting for fpp.rtp"));
	CHECK(std::stoi(still->substr(std::string("node fpp still configuring after ").size())) >= 2);
	CHECK(out.find("node fpp running") == std::string::npos);

	Rpc rpc(ear.port());
	const json graph = rpc.call("graph.get")["result"];
	INFO(graph.dump());
	const json fpp = nodeNamed(graph, "fpp");
	CHECK(fpp["state"] == "starting");
	CHECK(fpp["phase"] == "configuring");
	CHECK(fpp["waitingFor"] == json::array({{{"kind", "companion"}, {"name", "fpp.rtp"}}}));
	CHECK(ear.stop(rpc) == 0);
}

TEST_CASE("A hand-written node white noise disabled boots nothing and is an ear/bad-directive with the hint",
          "[system][lifecycle][graph-file]") {
	TempDirectory directory;
	writeGraphFile(directory, replacedOnce(noiseToRec(), "node white noise;", "node white noise disabled;"));
	Ear ear(directory, {"graph-file error ", true});
	INFO("ear printed:\n" << ear.process().output());
	const auto error = ear.process().lineStartingWith("graph-file error ");
	REQUIRE(error);
	CHECK(*error == "graph-file error 3:1 ear/bad-directive: node takes a type and a name: node <type> <name> [{ "
	                "<setting> <value>; ... }]; to disable a node, write disabled true; inside its braces");
	Rpc rpc(ear.port());
	CHECK(rpc.call("graph.get")["result"]["nodes"].empty());
	const json file = rpc.call("graphFile.get")["result"];
	INFO(file.dump());
	REQUIRE(file["diagnostics"].size() == 1);
	CHECK(file["diagnostics"][0]["code"] == "ear/bad-directive");
	CHECK(file["diagnostics"][0]["line"] == 3);
	CHECK_FALSE(file.contains("running"));
	CHECK(ear.stop(rpc) == 0);
	CHECK(ear.process().output().find("node noise") == std::string::npos);
	CHECK_FALSE(std::filesystem::exists(directory.recordings() / "rec.wav"));
}
