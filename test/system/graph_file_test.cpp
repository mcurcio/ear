// ear.conf end to end (docs/design/knobly-config.md, "Tracer bullet" and S5 criteria T1-T8): the
// real out/ear boots from a graph file folder; every request goes through the graph file keeper;
// editor edits are made at the source and written back, and a hand edit wins; a file with errors, or
// one the graph refuses, runs nothing and ear stays up; the folder's limits hold. Every wait is
// for an event (a line ear prints, a message, ear's exit); a deadline only fails a test.
#include <chrono>
#include <csignal>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ear_system.hpp"

namespace {

using namespace ear::test;

/// The record's example (docs/design/knobly-config.md, "File format") at `rate`: noise -> rec:record.left,
/// and two cards ear cannot open, desk (with a route) and porch.
std::string recordExample(std::uint32_t rate = 48000) {
	return "graph_frequency " + std::to_string(rate) + R"(;
block_size 256;
@set $ch 2;                      # comments survive every edit
@snippet card_out $id {          # written once, used by both cards
    card $id;
    playback $ch;
}
node white noise;
node file rec { path "take1.wav"; }
node card desk { @use card_out MOTU; }
node card porch { @use card_out USB2; }
route noise:noise.mono rec:record.left;
route noise:noise.mono desk:play.1;
)";
}

const std::string commentLine = "@set $ch 2;                      # comments survive every edit";

std::string replaced(std::string text, const std::string& from, const std::string& to) {
	const auto at = text.find(from);
	REQUIRE(at != std::string::npos);
	return text.replace(at, from.size(), to);
}

bool isFileChange(const json& message) { return message.value("method", "") == "graphFile.changed"; }

bool isNodeEvent(const json& message) { return message.value("method", "").starts_with("node."); }

/// The first graphFile.changed at or after `from`, read as it arrives.
json nextFileChange(Rpc& rpc, std::size_t from) { return rpc.waitFor(isFileChange, from); }

/// The diagnostic codes of a GraphFile's JSON.
std::vector<std::string> codesOf(const json& file) {
	std::vector<std::string> codes;
	for (const auto& d : file["diagnostics"]) codes.push_back(d["code"].get<std::string>());
	return codes;
}

/// Nothing ear sent or printed holds `text`.
void checkNowhere(const std::string& text, const Rpc& rpc, const ChildProcess& process,
                  const std::vector<json>& replies) {
	CHECK(json(rpc.notifications).dump().find(text) == std::string::npos);
	CHECK(json(replies).dump().find(text) == std::string::npos);
	CHECK(process.output().find(text) == std::string::npos);
}

/// Ready once the keeper's first offer is told (its status line), whatever it says.
const Ear::Ready firstOffer{"graph-file disk ", true};

} // namespace

TEST_CASE("T1: ear boots from the record's example, and the cards fail and keep their routes", "[system][graph-file]") {
	TempDirectory directory;
	writeGraphFile(directory, recordExample(44100));   // not the flags' default, so the file's rate shows
	Ear ear(directory, {linkedLine("noise:noise.mono", "rec:record.left")});
	auto& process = ear.process();
	INFO("ear printed:\n" << process.output());
	CHECK(process.waitForLineStartingWith("node desk failed engine-refused: ", deadline));
	CHECK(process.waitForLineStartingWith("node porch failed engine-refused: ", deadline));

	Rpc rpc(ear.port());
	const json graph = rpc.call("graph.get")["result"];
	INFO(graph.dump());
	CHECK(graph["graphFrequency"] == 44100);
	CHECK(graph["blockSize"] == 256);
	REQUIRE(graph["nodes"].size() == 4);
	CHECK(graph["nodes"][0]["name"] == "desk");
	CHECK(graph["nodes"][0]["settings"] == json{{"card", "MOTU"}, {"playback", 2}});
	CHECK(graph["nodes"][0]["state"] == "failed");
	CHECK(graph["nodes"][2]["name"] == "porch");
	CHECK(graph["nodes"][2]["state"] == "failed");
	REQUIRE(graph["routes"].size() == 2);
	CHECK(graph["routes"][0]["to"] == json{{"node", "desk"}, {"port", "play.1"}});
	CHECK(graph["routes"][1]["to"] == json{{"node", "rec"}, {"port", "record.left"}});
	CHECK(graph["routes"][1]["status"] == "linked");

	// The recording window: the noise check needs at least half of it.
	std::this_thread::sleep_for(1s);
	CHECK(ear.stop(rpc) == 0);
	CHECK(process.hasLine("node rec stopped dropped=0"));
	checkNoiseOnOneChannel(directory.recordings() / "take1.wav", 0, 44100);
}

TEST_CASE("T2: an editor edit is announced, applied, answered and written back, and a hand edit wins",
          "[system][graph-file]") {
	TempDirectory directory;
	writeGraphFile(directory, recordExample());
	Ear ear(directory, {linkedLine("noise:noise.mono", "rec:record.left")});
	INFO("ear printed:\n" << ear.process().output());
	Rpc rpc(ear.port());
	REQUIRE(rpc.call("graph.subscribe").contains("result"));
	REQUIRE(rpc.call("graphFile.subscribe").contains("result"));

	std::size_t from = rpc.notifications.size();
	const json reply = rpc.call("node.update", {{"name", "rec"}, {"settings", {{"path", "take2.wav"}}}});
	INFO("notifications: " << json(rpc.notifications).dump());
	CHECK(reply["result"] == json::object());
	const std::size_t changed = rpc.indexOf("graphFile.changed", {{"cause", "edit"}}, from);
	const std::size_t updated = rpc.indexOf("node.updated", {{"name", "rec"}, {"settings", {{"path", "take2.wav"}}}}, from);
	REQUIRE(changed < rpc.notifications.size());   // both before the reply
	REQUIRE(updated < rpc.notifications.size());
	CHECK(changed < updated);
	const std::string version = rpc.notifications[changed]["params"]["version"].get<std::string>();

	CHECK(ear.process().waitForLine("graph-file written " + version, deadline));
	const std::string written = readText(directory.graphFile());
	CHECK(written.find("path \"take2.wav\";") != std::string::npos);
	CHECK(written.find(commentLine) != std::string::npos);
	CHECK(written == rpc.notifications[changed]["params"]["text"]);

	// A hand edit, saved by rename: the next file change is the disk's, then the graph follows it.
	from = rpc.notifications.size();
	const std::string hand = replaced(written, "take2.wav", "take3.wav");
	writeGraphFile(directory, hand);
	const json disk = nextFileChange(rpc, from);
	CHECK(disk["params"]["cause"] == "disk");
	CHECK(disk["params"]["text"] == hand);
	CHECK(rpc.indexOf("graphFile.changed", json::object(), from) == rpc.indexOf("graphFile.changed", {{"cause", "disk"}}, from));
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.updated", {{"name", "rec"}, {"settings", {{"path", "take3.wav"}}}}); },
	            from);
	CHECK(ear.stop(rpc) == 0);
	CHECK(readText(directory.graphFile()) == hand);
}

TEST_CASE("T3: a hand typo stops edits until a fixing graphFile.set, and the graph keeps running",
          "[system][graph-file]") {
	TempDirectory directory;
	writeGraphFile(directory, recordExample());
	Ear ear(directory, {linkedLine("noise:noise.mono", "rec:record.left")});
	INFO("ear printed:\n" << ear.process().output());
	Rpc rpc(ear.port());
	REQUIRE(rpc.call("graph.subscribe").contains("result"));
	REQUIRE(rpc.call("graphFile.subscribe").contains("result"));

	std::size_t from = rpc.notifications.size();
	writeGraphFile(directory, replaced(recordExample(), R"(path "take1.wav"; })", R"(path "take1.wav" })"));
	const json typo = nextFileChange(rpc, from);
	INFO("notifications: " << json(rpc.notifications).dump());
	CHECK(typo["params"]["cause"] == "disk");
	REQUIRE_FALSE(typo["params"]["diagnostics"].empty());
	CHECK(typo["params"]["diagnostics"][0]["severity"] == "error");
	CHECK(ear.process().waitForLineStartingWith("graph-file error ", deadline));

	const json refused = rpc.call("node.update", {{"name", "rec"}, {"settings", {{"path", "take2.wav"}}}});
	CHECK(refused["error"]["data"]["code"] == "file-invalid");
	for (std::size_t i = from; i < rpc.notifications.size(); ++i) CHECK_FALSE(isNodeEvent(rpc.notifications[i]));

	from = rpc.notifications.size();
	const json fixed = rpc.call("graphFile.set", {{"text", replaced(recordExample(), "take1.wav", "take2.wav")},
	                                               {"version", typo["params"]["version"]}});
	CHECK(fixed["result"] == json::object());
	CHECK(rpc.indexOf("node.updated", {{"name", "rec"}, {"settings", {{"path", "take2.wav"}}}}, from) <
	      rpc.notifications.size());
	CHECK(ear.stop(rpc) == 0);
}

TEST_CASE("T4: ear.conf as a symlink out is unreadable, and nothing is written through it", "[system][graph-file]") {
	TempDirectory directory;
	const std::string secret = "node white secret_outside_text;\n";
	const auto outside = directory.path() / "outside.conf";
	std::ofstream(outside) << secret;
	std::filesystem::create_directories(directory.config());
	std::filesystem::create_symlink("../outside.conf", directory.graphFile());
	Ear ear(directory, firstOffer);
	INFO("ear printed:\n" << ear.process().output());
	Rpc rpc(ear.port());
	std::vector<json> replies;

	replies.push_back(rpc.call("graphFile.subscribe"));
	const json file = replies.back()["result"];
	CHECK(file["text"] == "");
	CHECK(codesOf(file) == std::vector<std::string>{"ear/unreadable-file"});

	const std::size_t from = rpc.notifications.size();
	replies.push_back(rpc.call("graphFile.set", {{"text", graphFileText("node white n;\n")}, {"version", file["version"]}}));
	INFO("replies: " << json(replies).dump() << "\nnotifications: " << json(rpc.notifications).dump());
	CHECK(replies.back()["result"] == json::object());
	const json edit = nextFileChange(rpc, from);
	CHECK(edit["params"].contains("writeFailed"));
	replies.push_back(rpc.call("graphFile.get"));
	CHECK(replies.back()["result"].contains("writeFailed"));

	CHECK(ear.stop(rpc) == 0);
	CHECK(readText(outside) == secret);
	CHECK(std::filesystem::is_symlink(directory.graphFile()));
	checkNowhere("secret_outside_text", rpc, ear.process(), replies);
}

TEST_CASE("T4: an @include of a file outside is refused without reading it", "[system][graph-file]") {
	TempDirectory directory;
	std::ofstream(directory.path() / "outside.conf") << "node white secret_outside_text;\n";
	writeGraphFile(directory, noiseToRec("record.left", "@include \"../outside.conf\";\n"));
	Ear ear(directory, firstOffer);
	INFO("ear printed:\n" << ear.process().output());
	Rpc rpc(ear.port());
	std::vector<json> replies{rpc.call("graphFile.get"), rpc.call("graph.get")};
	CHECK(codesOf(replies[0]["result"]) == std::vector<std::string>{"ear/include-unsupported"});
	CHECK(replies[1]["result"]["nodes"].empty());
	CHECK(ear.stop(rpc) == 0);
	checkNowhere("secret_outside_text", rpc, ear.process(), replies);
}

TEST_CASE("T5: a boot with a typo runs nothing, shows the diagnostic and stops cleanly", "[system][graph-file]") {
	TempDirectory directory;
	writeGraphFile(directory, replaced(noiseToRec(), "node white noise;", "node white noise"));
	Ear ear(directory, {"graph-file error ", true});
	INFO("ear printed:\n" << ear.process().output());
	Rpc rpc(ear.port());
	CHECK(rpc.call("graph.get")["result"]["nodes"].empty());
	const json file = rpc.call("graphFile.get")["result"];
	INFO(file.dump());
	REQUIRE_FALSE(file["diagnostics"].empty());
	CHECK(file["diagnostics"][0]["severity"] == "error");
	CHECK_FALSE(file.contains("running"));
	CHECK(ear.stop(rpc) == 0);
}

TEST_CASE("T6: a boot with a route to an undeclared port runs nothing and shows where", "[system][graph-file]") {
	TempDirectory directory;
	writeGraphFile(directory, noiseToRec("FX"));   // line 5: route noise:noise.mono rec:FX;
	Ear ear(directory, {"graph-file refused unknown-port at 5:1"});
	INFO("ear printed:\n" << ear.process().output());
	Rpc rpc(ear.port());
	CHECK(rpc.call("graph.get")["result"]["nodes"].empty());
	const json file = rpc.call("graphFile.get")["result"];
	INFO(file.dump());
	CHECK(file["refusal"] == json{{"code", "unknown-port"}, {"message", "unknown-port rec:FX"}, {"line", 5}, {"column", 1}});
	CHECK_FALSE(file.contains("running"));
	CHECK(ear.stop(rpc) == 0);
	CHECK_FALSE(std::filesystem::exists(directory.recordings() / "rec.wav"));
}

TEST_CASE("A dotted port goes through the whole path: file, graph, engine, levels, status lines and the wire", "[system][graph-file]") {
	TempDirectory directory;
	const std::string text = noiseToRec();
	writeGraphFile(directory, text);
	Ear ear(directory, {linkedLine("noise:noise.mono", "rec:record.left")});
	INFO("ear printed:\n" << ear.process().output());
	Rpc rpc(ear.port());

	const json graph = rpc.call("graph.get")["result"];
	INFO(graph.dump());
	CHECK(graph["nodes"][0]["outputs"] == json{"noise.mono"});
	CHECK(graph["nodes"][1]["inputs"] == json{"record.left", "record.right"});
	CHECK(graph["routes"][0]["from"] == json{{"node", "noise"}, {"port", "noise.mono"}});
	CHECK(graph["routes"][0]["to"] == json{{"node", "rec"}, {"port", "record.left"}});

	// An edit through the wire is linked in the engine and written to the file bare.
	const json added = rpc.call("route.add", {{"from", {{"node", "noise"}, {"port", "noise.mono"}}},
	                                          {"to", {{"node", "rec"}, {"port", "record.right"}}}});
	CHECK(added["result"] == json::object());
	CHECK(ear.process().waitForLine(linkedLine("noise:noise.mono", "rec:record.right"), deadline));
	CHECK(ear.process().waitForLineStartingWith("graph-file written ", deadline));
	CHECK(readText(directory.graphFile()) == text + "route noise:noise.mono rec:record.right;\n");

	CHECK(rpc.call("levels.subscribe").contains("result"));
	const json levels = rpc.waitFor([](const json& m) {
		return m.value("method", "") == "levels" && m["params"]["nodes"].contains("rec") &&
		       m["params"]["nodes"]["rec"].contains("record.right") && m["params"]["nodes"]["rec"]["record.right"][0] > 0;
	});
	INFO(levels.dump());
	CHECK(levels["params"]["nodes"]["noise"].contains("noise.mono"));
	CHECK(ear.stop(rpc) == 0);
}

TEST_CASE("An old port name is the ordinary unknown-port refusal, and nothing runs", "[system][graph-file]") {
	// Each type's old names on a graph that is otherwise right, so only the name can refuse it.
	const auto [nodes, route, refused] = GENERATE(table<std::string, std::string, std::string>({
	    {"", "route noise:MONO rec:record.left;", "noise:MONO"},
	    {"", "route noise:noise.mono rec:FL;", "rec:FL"},
	    {"", "route noise:noise.mono rec:FR;", "rec:FR"},
	    {"node card desk { card Test; playback 1; }", "route noise:noise.mono desk:playback_1;", "desk:playback_1"},
	    {"node card desk { card Test; capture 1; }", "route desk:capture_1 rec:record.left;", "desk:capture_1"},
	    {"node rtp fpp { address \"0.0.0.0\"; port 5004; format L24; channels 1; }", "route fpp:channel_1 rec:record.left;", "fpp:channel_1"},
	    {"node sendspin kitchen;", "route kitchen:from_server_FL rec:record.left;", "kitchen:from_server_FL"},
	    {"node sendspin kitchen;", "route noise:noise.mono kitchen:to_server_FR;", "kitchen:to_server_FR"},
	    // The agreed name's parts the other way round, and a bank with no member, are no names either.
	    {"", "route noise:mono.noise rec:record.left;", "noise:mono.noise"},
	    {"", "route noise:noise rec:record.left;", "noise:noise"},
	}));
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node white noise;\nnode file rec { path \"rec.wav\"; }\n" + nodes + "\n" + route + "\n"));
	Ear ear(directory, {"graph-file refused unknown-port at 6:1"});
	INFO("ear printed:\n" << ear.process().output());
	Rpc rpc(ear.port());
	CHECK(rpc.call("graph.get")["result"]["nodes"].empty());
	CHECK(rpc.call("graphFile.get")["result"]["refusal"]["message"] == "unknown-port " + refused);
	CHECK(ear.stop(rpc) == 0);
}

TEST_CASE("T7: without ear.conf the flags set the engine, and graphFile.set makes the file", "[system][graph-file]") {
	TempDirectory directory;
	Ear ear(directory, {"graph-file error 1:1 ear/missing-file", true}, {"--graph-frequency", "44100"});
	INFO("ear printed:\n" << ear.process().output());
	Rpc rpc(ear.port());
	const json graph = rpc.call("graph.get")["result"];
	CHECK(graph["graphFrequency"] == 44100);
	CHECK(graph["blockSize"] == 256);
	REQUIRE(rpc.call("graphFile.subscribe").contains("result"));
	const json file = rpc.call("graphFile.get")["result"];
	CHECK(file["text"] == "");
	CHECK(codesOf(file) == std::vector<std::string>{"ear/missing-file"});

	const std::size_t from = rpc.notifications.size();
	const std::string text = noiseToRec();
	const json set = rpc.call("graphFile.set", {{"text", text}, {"version", file["version"]}});
	INFO("notifications: " << json(rpc.notifications).dump());
	CHECK(set["result"] == json::object());
	const json edit = nextFileChange(rpc, from);
	CHECK(edit["params"]["pendingRestart"] == json::array({"graphFrequency"}));
	CHECK(ear.process().waitForLine("graph-file written " + edit["params"]["version"].get<std::string>(), deadline));
	CHECK(ear.process().waitForLine(linkedLine("noise:noise.mono", "rec:record.left"), deadline));
	CHECK(readText(directory.graphFile()) == text);
	CHECK(ear.stop(rpc) == 0);
}

TEST_CASE("T7: ear.conf without block_size runs nothing and the engine takes the flags'", "[system][graph-file]") {
	TempDirectory directory;
	writeGraphFile(directory, "graph_frequency 44100;\nnode white noise;\n");
	Ear ear(directory, firstOffer);
	INFO("ear printed:\n" << ear.process().output());
	Rpc rpc(ear.port());
	const json graph = rpc.call("graph.get")["result"];
	CHECK(graph["graphFrequency"] == 48000);
	CHECK(graph["blockSize"] == 256);
	CHECK(graph["nodes"].empty());
	CHECK(codesOf(rpc.call("graphFile.get")["result"]) == std::vector<std::string>{"ear/missing-directive"});
	CHECK(ear.stop(rpc) == 0);
}

TEST_CASE("T8: a graph file folder that is or is inside the recordings folder exits 1, naming both",
          "[system][graph-file]") {
	TempDirectory directory;
	std::filesystem::create_directories(directory.recordings());   // so the inner folder can be made
	for (const auto& config : {directory.recordings(), directory.recordings() / "config"}) {
		INFO(config);
		ChildProcess ear(EAR_BINARY, {"--config-dir", config.string(), "--recordings-dir", directory.recordings().string(),
		                              "--control-port", "0"});
		const auto exitStatus = ear.waitForExit(deadline);
		INFO("ear printed:\n" << ear.output());
		CHECK(exitStatus == 1);
		CHECK(ear.output().find(config.string()) != std::string::npos);
		CHECK(ear.output().find(directory.recordings().string() + " ") != std::string::npos);
		CHECK(ear.output().find("overlap") != std::string::npos);
	}
}

TEST_CASE("T8: a graph file or recordings folder that is or is inside the web root exits 1, naming both",
          "[system][graph-file]") {
	TempDirectory directory;
	const auto web = directory.path() / "web";
	std::filesystem::create_directories(web);   // a web root that is not there is not compared
	struct Case { std::filesystem::path config; std::filesystem::path recordings; std::filesystem::path inside; };
	for (const auto& [config, recordings, inside] : {Case{web / "config", directory.recordings(), web / "config"},
	                                                  Case{directory.config(), web, web}}) {
		INFO(inside);
		ChildProcess ear(EAR_BINARY, {"--config-dir", config.string(), "--recordings-dir", recordings.string(),
		                              "--web-root", web.string(), "--control-port", "0"});
		const auto exitStatus = ear.waitForExit(deadline);
		INFO("ear printed:\n" << ear.output());
		CHECK(exitStatus == 1);
		CHECK(ear.output().find(inside.string() + " ") != std::string::npos);
		CHECK(ear.output().find(" " + web.string() + " overlap") != std::string::npos);
	}
}

TEST_CASE("A --graph-frequency or --block-size that is not a whole number above 0 exits 1", "[system][graph-file]") {
	TempDirectory directory;
	for (const auto& [flag, value] : {std::pair{"--graph-frequency", "0"}, std::pair{"--block-size", "0"},
	                                  std::pair{"--graph-frequency", "-48000"}, std::pair{"--block-size", "4294967296"}}) {
		INFO(flag << ' ' << value);
		ChildProcess ear(EAR_BINARY, earArguments(directory, {flag, value}));
		const auto exitStatus = ear.waitForExit(deadline);
		INFO("ear printed:\n" << ear.output());
		CHECK(exitStatus == 1);
		CHECK(ear.output().find("--graph-frequency and --block-size must be whole numbers above 0") != std::string::npos);
	}
}

TEST_CASE("Edits holding a control character ear.conf cannot hold are refused, and ear stays up", "[system][graph-file]") {
	TempDirectory directory;
	writeGraphFile(directory, noiseToRec());
	Ear ear(directory, {linkedLine("noise:noise.mono", "rec:record.left")});
	INFO("ear printed:\n" << ear.process().output());
	Rpc rpc(ear.port());
	const std::string nul("x\0y", 3);   // "\u0000" on the wire; each of these once ended ear (exit 134)
	const auto code = [](const json& reply) { return reply["error"]["data"]["code"]; };
	CHECK(code(rpc.call("node.update", {{"name", "rec"}, {"settings", {{"path", nul}}}})) == "invalid-settings");
	CHECK(code(rpc.call("node.add", {{"name", "rec2"}, {"type", "file"}, {"settings", {{"path", "a\x01" "b"}}}})) ==
	      "invalid-settings");
	CHECK(code(rpc.call("route.add", {{"from", {{"node", "noise"}, {"port", "MO" + nul}}}, {"to", {{"node", "rec"}, {"port", "record.right"}}}})) ==
	      "unknown-port");
	CHECK(rpc.call("graph.get")["result"]["nodes"].size() == 2);
	CHECK(readText(directory.graphFile()) == noiseToRec());
	CHECK(ear.stop(rpc) == 0);
}
