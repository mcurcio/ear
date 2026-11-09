// The property table (engine_properties.hpp, docs/engine.md#properties-ear-sets) against what it describes: the live
// engine, engine.conf, the tests its rows name, and docs/engine.md's tables. The compile-time half is the table's own
// static_asserts, property<home>() and the PropertyKey<home> type, which compile_fail/engine_properties_bad.cpp shows
// refusing an unlisted key and a key of another home. The live
// check makes one of every object ear sets properties on (the context, the internal clock, two port blocks and their
// ports, a route link, a sound card from the test ALSA configuration with the test seam's clock.name, an RTP receiver
// and both companions' links) and reads each object's properties as PipeWire holds them, through the context's own
// objects (impl API), not what ear asked for. Every wait is event-gated; the deadline only fails the test.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pipewire/impl.h>
#include <pipewire/pipewire.h>
#include <spa/utils/json.h>

#include "../engine_properties.hpp"
#include "../engine_property_set.hpp"
#include "catch2/catch.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/links.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/rtp_receivers.hpp"
#include "ear/pipewire/sound_cards.hpp"
#include "ear/testing/drive_now.hpp"
#include "engine_harness.hpp"
#include "rtp_tone.hpp"
#include "temp_folder.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace ear::pipewire::testing;
using namespace std::chrono_literals;

using Held = std::map<std::string, std::string, std::less<>>;

Held held(const spa_dict* dict) {
	Held found;
	if (dict == nullptr) return found;
	const spa_dict_item* item = nullptr;
	spa_dict_for_each(item, dict) found[item->key] = item->value != nullptr ? item->value : "";
	return found;
}

/// An object, or a string of one, as PipeWire's own parser reads it (pw_properties_new_string).
Held parsed(const std::string& text) {
	pw_properties* props = pw_properties_new_string(text.c_str());
	REQUIRE(props != nullptr);
	Held found = held(&props->dict);
	pw_properties_free(props);
	return found;
}

std::string readFile(const std::filesystem::path& file) {
	std::ifstream in(file);
	REQUIRE(in.good());
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}

/// Each object of an array, as PipeWire's own parser reads it.
std::vector<Held> elements(const std::string& array) {
	std::vector<Held> found;
	spa_json it;
	REQUIRE(spa_json_begin_array(&it, array.c_str(), array.size()) > 0);
	const char* value = nullptr;
	for (int length = 0; (length = spa_json_next(&it, &value)) > 0;) {
		if (spa_json_is_container(value, length) != 0) length = spa_json_container_len(&it, value, length);
		found.push_back(parsed(std::string(value, static_cast<std::size_t>(length))));
	}
	return found;
}

std::set<std::string> keysOf(const Held& props) {
	std::set<std::string> keys;
	for (const auto& [key, value] : props) keys.insert(key);
	return keys;
}

std::set<std::string> tableKeys(Home home) {
	std::set<std::string> keys;
	for (const PropertyRow& row : engineProperties)
		if (row.home == home) keys.emplace(row.key);
	return keys;
}

// ---- The live engine.

/// Every global of `type` in the engine's context: the objects PipeWire holds, synchronously, on the main loop.
std::vector<pw_global*> globalsOf(Engine& engine, const char* type) {
	struct Search {
		const char* type;
		std::vector<pw_global*> found;
	} search{type, {}};
	pw_context_for_each_global(
		engine.context(),
		[](void* data, pw_global* global) {
			auto* self = static_cast<Search*>(data);
			if (pw_global_is_type(global, self->type)) self->found.push_back(global);
			return 0;
		},
		&search);
	return search.found;
}

std::optional<Held> nodeHeld(Engine& engine, std::string_view name) {
	for (pw_global* global : globalsOf(engine, PW_TYPE_INTERFACE_Node)) {
		Held props = held(&pw_impl_node_get_properties(static_cast<pw_impl_node*>(pw_global_get_object(global)))->dict);
		if (props[PW_KEY_NODE_NAME] == name) return props;
	}
	return std::nullopt;
}

struct HeldPort {
	std::uint32_t id;
	Held props;
};

std::optional<HeldPort> portHeld(Engine& engine, std::uint32_t nodeId, std::string_view name) {
	for (pw_global* global : globalsOf(engine, PW_TYPE_INTERFACE_Port)) {
		Held props = held(&pw_impl_port_get_properties(static_cast<pw_impl_port*>(pw_global_get_object(global)))->dict);
		if (props[PW_KEY_NODE_ID] == std::to_string(nodeId) && props[PW_KEY_PORT_NAME] == name)
			return HeldPort{pw_global_get_id(global), std::move(props)};
	}
	return std::nullopt;
}

/// The names of the ports of node `nodeId`, as PipeWire holds them.
std::set<std::string> portNamesOf(Engine& engine, std::uint32_t nodeId) {
	std::set<std::string> names;
	for (pw_global* global : globalsOf(engine, PW_TYPE_INTERFACE_Port)) {
		Held props = held(&pw_impl_port_get_properties(static_cast<pw_impl_port*>(pw_global_get_object(global)))->dict);
		if (props[PW_KEY_NODE_ID] == std::to_string(nodeId)) names.insert(props[PW_KEY_PORT_NAME]);
	}
	return names;
}

/// The name of port `portId` if it is a port of node `nodeId`; otherwise a text no port name equals.
std::string portNameOn(Engine& engine, const std::string& nodeId, const std::string& portId) {
	for (pw_global* global : globalsOf(engine, PW_TYPE_INTERFACE_Port)) {
		if (std::to_string(pw_global_get_id(global)) != portId) continue;
		Held props = held(&pw_impl_port_get_properties(static_cast<pw_impl_port*>(pw_global_get_object(global)))->dict);
		return props[PW_KEY_NODE_ID] == nodeId ? props[PW_KEY_PORT_NAME]
		                                       : "<a port of node " + props[PW_KEY_NODE_ID] + ">";
	}
	return "<no port " + portId + ">";
}

/// Every link from node `output` to node `input`, selected by those nodes' ids alone, with each port id PipeWire holds
/// replaced by the name of the port it is on that node: what the row's value says, read from what is held.
std::vector<Held> linksBetween(Engine& engine, std::uint32_t output, std::uint32_t input) {
	std::vector<Held> found;
	for (pw_global* global : globalsOf(engine, PW_TYPE_INTERFACE_Link)) {
		const pw_link_info* info = pw_impl_link_get_info(static_cast<pw_impl_link*>(pw_global_get_object(global)));
		if (info == nullptr || info->output_node_id != output || info->input_node_id != input) continue;
		Held props = held(info->props);
		for (const char* side : {"output", "input"}) {
			const std::string port = std::string("link.") + side + ".port";
			const std::string node = std::string("link.") + side + ".node";
			if (props.contains(port) && props.contains(node))
				props[port] = portNameOn(engine, props.at(node), props.at(port));
		}
		found.push_back(std::move(props));
	}
	return found;
}

/// The arguments of the loaded module `name`, as PipeWire parses them.
std::optional<Held> moduleArgumentsHeld(Engine& engine, std::string_view name) {
	for (pw_global* global : globalsOf(engine, PW_TYPE_INTERFACE_Module)) {
		const pw_module_info* info =
			pw_impl_module_get_info(static_cast<pw_impl_module*>(pw_global_get_object(global)));
		if (info != nullptr && info->name != nullptr && name == info->name && info->args != nullptr)
			return parsed(info->args);
	}
	return std::nullopt;
}

/// Writes zeros; the blocks need code to run.
class Silence final : public AudioCode {
public:
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
	}
	void release() noexcept override {}
};

class CompanionLog final : public EngineCompanionEvents {
public:
	explicit CompanionLog(Waiter& waiter) : waiter_(waiter) {}
	void ready() override {
		isReady = true;
		waiter_.wake();
	}
	void lost(std::string detail) override {
		lostDetail = std::move(detail);
		waiter_.wake();
	}
	bool settled() const { return isReady || lostDetail.has_value(); }
	bool isReady = false;
	std::optional<std::string> lostDetail;

private:
	Waiter& waiter_;
};

class LinkLog final : public LinkListener {
public:
	explicit LinkLog(Waiter& waiter) : waiter_(waiter) {}
	void linkActive(LinkId) override {
		active = true;
		waiter_.wake();
	}
	void linkFailed(LinkId, std::string detail) override {
		failed = std::move(detail);
		waiter_.wake();
	}
	void linkGone(LinkId) override { waiter_.wake(); }
	bool active = false;
	std::optional<std::string> failed;

private:
	Waiter& waiter_;
};

/// One object a home's rows are read on: what PipeWire holds; the value expected where the row's own value says how it
/// is computed rather than what it is; the keys PipeWire consumes before this object (each read on another instance
/// of the home, and its effect checked in the test); keys PipeWire holds under another name; keys `forbidden` refuses
/// to ear that PipeWire itself sets here; and whether the object is judged against `forbidden` at all.
struct Instance {
	std::string what;
	Held props;
	std::map<std::string, std::string, std::less<>> expected;
	std::set<std::string, std::less<>> consumed;
	std::map<std::string, std::string, std::less<>> heldAs;
	std::set<std::string, std::less<>> setByPipeWire;
	bool judgeForbidden = true;
};

// ---- docs/engine.md.

std::string trim(std::string_view text) {
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
	return std::string(text);
}

/// A cell as the table's text: backticks and enclosing quotes gone.
std::string plain(std::string_view cell) {
	std::string text;
	for (const char c : trim(cell))
		if (c != '`') text += c;
	if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
	return text;
}

std::vector<std::string> cells(const std::string& line) {
	std::vector<std::string> found;
	std::string cell;
	for (std::size_t i = 1; i < line.size(); ++i) {
		if (line[i] == '|') {
			found.push_back(cell);
			cell.clear();
		} else {
			cell += line[i];
		}
	}
	return found;
}

std::vector<std::string> splitList(const std::string& cell) {
	std::vector<std::string> items;
	std::stringstream in(cell);
	for (std::string item; std::getline(in, item, ',');) items.push_back(plain(item));
	return items;
}

/// GitHub's anchor for a heading.
std::string slug(std::string_view heading) {
	std::string anchor;
	for (const char c : trim(heading)) {
		if (std::isalnum(static_cast<unsigned char>(c)) != 0)
			anchor += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		else if (c == ' ' || c == '-') anchor += '-';
		else if (c == '_') anchor += '_';
	}
	return anchor;
}

struct Doc {
	std::set<std::string> anchors;
	std::vector<std::vector<std::string>> setRows;
	std::vector<std::vector<std::string>> neverRows;
};

/// The rows of the first table under each of the two headings.
Doc readDoc() {
	std::istringstream in(readFile(EAR_ENGINE_DOC));
	Doc doc;
	std::vector<std::vector<std::string>>* table = nullptr;
	bool inTable = false;
	bool inFence = false;
	for (std::string line; std::getline(in, line);) {
		if (line.starts_with("```")) inFence = !inFence;
		if (inFence) continue;
		if (line.starts_with("#")) {
			const std::string heading = line.substr(line.find_first_not_of('#'));
			doc.anchors.insert(slug(heading));
			table = nullptr;
			inTable = false;
			if (trim(heading) == "Properties ear sets") table = &doc.setRows;
			if (trim(heading) == "Properties ear never sets") table = &doc.neverRows;
			continue;
		}
		if (table == nullptr) continue;
		if (!line.starts_with("|")) {
			if (inTable) table = nullptr; // the first table only
			continue;
		}
		if (!inTable) { // the header row, then the separator
			inTable = true;
			std::getline(in, line);
			continue;
		}
		table->push_back(cells(line));
	}
	return doc;
}

std::optional<Home> homeNamed(std::string_view name) {
	for (const Home home : allHomes)
		if (homeName(home) == name) return home;
	return std::nullopt;
}

} // namespace

TEST_CASE("Every property ear sets is held by PipeWire as the property table says", "[engine][properties]") {
	ear::test::TempFolder alsaOut{"properties"};
	setenv("ALSA_CONFIG_PATH", EAR_ALSA_TEST_DIR "/alsa.conf", 1);
	setenv("EAR_ALSA_OUT", alsaOut.path().c_str(), 1);
	auto engine = makeEngine();
	Waiter waiter(*engine);
	NoBell bell;
	FilterPortBlocks blocks(*engine, bell);
	AlsaSoundCards cards(*engine, clocklessCardProperties());
	RtpReceivers receivers(*engine);
	Links links(*engine);
	LinkLog linkLog(waiter);
	links.subscribe(linkLog);
	Silence cardCode;
	Silence fppCode;
	BlockEvents cardEvents(waiter);
	BlockEvents fppEvents(waiter);
	auto cardBlock = blocks.make("card",
	                             Ports{{{Direction::input, "play", {"1", "2"}},
	                                    {Direction::output, "to_card", {"1", "2"}, PortBank::Reach::inside}}},
	                             kFormat, cardEvents);
	auto fppBlock = blocks.make("fpp",
	                            Ports{{{Direction::output, "stream", {"1", "2"}},
	                                   {Direction::input, "from_rtp", {"1", "2"}, PortBank::Reach::inside}}},
	                            kFormat, fppEvents);
	REQUIRE(ear::testing::driveNow(*cardBlock, &cardCode));
	REQUIRE(ear::testing::driveNow(*fppBlock, &fppCode));
	REQUIRE(waiter.until([&] { return cardEvents.isReady && fppEvents.isReady; }));
	const std::uint32_t cardId = cardBlock->blockId();
	const std::uint32_t fppId = fppBlock->blockId();

	const auto streamPort = portHeld(*engine, fppId, "stream.1");
	const auto playPort = portHeld(*engine, cardId, "play.1");
	REQUIRE(streamPort);
	REQUIRE(playPort);
	links.createLink(1, streamPort->id, playPort->id);
	REQUIRE(waiter.until([&] { return linkLog.active || linkLog.failed; }));
	REQUIRE(linkLog.failed.value_or("") == "");

	CompanionLog cardLog(waiter);
	auto card = cards.make(
		CompanionTarget{"card.playback", "card", Direction::output, {"to_card.1", "to_card.2"}, kFormat, cardId},
		SoundCardShape{*CardId::parse("Test")}, cardLog);
	CompanionLog receiverLog(waiter);
	const std::uint16_t udpPort = ear::test::freeUdpPort();
	auto receiver = receivers.make(
		CompanionTarget{"fpp.rtp", "fpp", Direction::input, {"from_rtp.1", "from_rtp.2"}, kFormat, fppId},
		RtpReceiverShape{*RtpAddress::parse("0.0.0.0"), udpPort, RtpEncoding::l24, kRate, 100}, receiverLog);
	REQUIRE(waiter.until([&] { return cardLog.settled() && receiverLog.settled(); }, 15s));
	REQUIRE(cardLog.lostDetail.value_or("") == "");
	REQUIRE(receiverLog.lostDetail.value_or("") == "");

	const auto cardNode = nodeHeld(*engine, "card.playback");
	const auto receiverNode = nodeHeld(*engine, "fpp.rtp");
	const auto dummy = nodeHeld(*engine, "Dummy-Driver");
	const auto cardBlockNode = nodeHeld(*engine, "card");
	const auto toCard = portHeld(*engine, cardId, "to_card.1");
	REQUIRE(cardNode);
	REQUIRE(receiverNode);
	REQUIRE(dummy);
	REQUIRE(cardBlockNode);
	REQUIRE(toCard);
	const std::uint32_t cardNodeId = static_cast<std::uint32_t>(std::stoul(cardNode->at(PW_KEY_OBJECT_ID)));
	const std::uint32_t receiverNodeId = static_cast<std::uint32_t>(std::stoul(receiverNode->at(PW_KEY_OBJECT_ID)));
	const std::vector<Held> routeLinks = linksBetween(*engine, fppId, cardId);
	const std::vector<Held> cardLinks = linksBetween(*engine, cardId, cardNodeId);
	const std::vector<Held> receiverLinks = linksBetween(*engine, receiverNodeId, fppId);
	REQUIRE(routeLinks.size() == 1);
	REQUIRE(cardLinks.size() == 2);
	REQUIRE(receiverLinks.size() == 2);
	// Each companion links every channel, each from its own port.
	auto outputPorts = [](const std::vector<Held>& found) {
		std::set<std::string> names;
		for (const Held& link : found)
			names.insert(link.contains("link.output.port") ? link.at("link.output.port") : "");
		return names;
	};
	CHECK(outputPorts(cardLinks) == std::set<std::string>{"to_card.1", "to_card.2"});
	CHECK(outputPorts(receiverLinks) == std::set<std::string>{"receive_AUX0", "receive_AUX1"});
	// adapter.auto-port-config's effect: the receiver's ports are exactly one per channel, no monitor or control port.
	CHECK(portNamesOf(*engine, receiverNodeId) == std::set<std::string>{"receive_AUX0", "receive_AUX1"});
	const auto rtpArguments = moduleArgumentsHeld(*engine, "libpipewire-module-rtp-source");
	REQUIRE(rtpArguments);

	const std::string rate = std::to_string(kRate);
	const std::string block = std::to_string(kBlock);
	auto number = [](std::uint32_t value) { return std::to_string(value); };
	std::map<Home, std::vector<Instance>> instances;
	// engine.conf's forbidden keys are the file's (judged by the conf test): the context holds Engine's default.clock.*.
	instances[Home::engineConf].push_back(
		{"the context", held(&pw_context_get_properties(engine->context())->dict), {}, {}, {}, {}, false});
	instances[Home::context].push_back({"the context",
	                                    held(&pw_context_get_properties(engine->context())->dict),
	                                    {{"config.name", EAR_ENGINE_CONF},
	                                     {"default.clock.rate", rate},
	                                     {"default.clock.allowed-rates", "[ " + rate + " ]"},
	                                     {"default.clock.quantum", block},
	                                     {"default.clock.min-quantum", block},
	                                     {"default.clock.max-quantum", block},
	                                     {"default.clock.quantum-floor", block},
	                                     {"default.clock.quantum-limit", block}}});
	instances[Home::internalClock].push_back({"Dummy-Driver", *dummy, {}});
	instances[Home::block].push_back({"block card", *cardBlockNode, {{"node.name", "card"}, {"media.name", "card"}}});
	instances[Home::blockPort].push_back({"port play.1", playPort->props, {{"port.name", "play.1"}}});
	instances[Home::blockPort].push_back({"port to_card.1", toCard->props, {{"port.name", "to_card.1"}}});
	const Held cardExpected{{"factory.name", "api.alsa.pcm.sink"},
	                        {"api.alsa.path", "hw:CARD=Test,DEV=0"},
	                        {"audio.channels", "2"},
	                        {"audio.rate", rate},
	                        {"audio.position", "[ AUX0 AUX1 ]"},
	                        {"node.name", "card.playback"},
	                        {"api.alsa.period-size", block},
	                        {"api.alsa.period-num", number(cardPeriods(kBlock))}};
	// The ALSA node publishes node.driver = true itself (docs/engine.md#properties-ear-never-sets).
	instances[Home::card].push_back({"card.playback", *cardNode, cardExpected, {}, {}, {"node.driver"}});
	instances[Home::cardSeam].push_back({"card.playback", *cardNode, {}});
	CHECK((cardNode->contains("node.driver") && cardNode->at("node.driver") == "true"));
	Held streamRows;
	for (const PropertyRow& row : engineProperties)
		if (row.home == Home::rtpStream) streamRows[std::string(row.key)] = row.value;
	const Held receiverExpected{{"source.ip", "0.0.0.0"},
	                            {"source.port", number(udpPort)},
	                            {"sess.latency.msec", "100"},
	                            {"audio.format", "S24BE"},
	                            {"audio.rate", rate},
	                            {"audio.channels", "2"},
	                            {"audio.position", "[ AUX0 AUX1 ]"},
	                            {"node.name", "fpp.rtp"},
	                            {"node.description", "fpp.rtp"},
	                            {"media.name", "fpp.rtp"}};
	// The receiver's node holds most arguments; source.ip and source.port as rtp.source.*; remote.name (the module's
	// connection) and stream.props (the node's own properties, read below) only the module's arguments hold.
	instances[Home::rtpReceiver].push_back({"fpp.rtp",
	                                        *receiverNode,
	                                        receiverExpected,
	                                        {"remote.name", "stream.props"},
	                                        {{"source.ip", "rtp.source.ip"}, {"source.port", "rtp.source.port"}}});
	instances[Home::rtpReceiver].push_back({"rtp-source's arguments", *rtpArguments, receiverExpected});
	// The module gets the table's receiver arguments and nothing else.
	CHECK(keysOf(*rtpArguments) == tableKeys(Home::rtpReceiver));
	// The stream's own adapter, on the module's connection, consumes adapter.auto-port-config: the engine's node never
	// holds it, and its effect is the receiver's port set checked above. The argument holds it.
	instances[Home::rtpStream].push_back(
		{"rtp-source's stream.props", parsed(rtpArguments->at("stream.props")), {}, {}});
	instances[Home::rtpStream].push_back({"fpp.rtp", *receiverNode, {}, {"adapter.auto-port-config"}});
	// Links are selected by their nodes' ids; each port row is the name of the port PipeWire's held id is on that node.
	instances[Home::link].push_back(
		{"the route link", routeLinks[0], {{"link.output.port", "stream.1"}, {"link.input.port", "play.1"}}});
	for (const Held& link : cardLinks) {
		const std::string output = link.contains("link.output.port") ? link.at("link.output.port") : "";
		const std::uint32_t channel = output.starts_with("to_card.") ? std::stoul(output.substr(8)) : 0;
		instances[Home::companionLink].push_back({"a card link from " + output,
		                                          link,
		                                          {{"link.output.node", number(cardId)},
		                                           {"link.output.port", "to_card." + number(channel)},
		                                           {"link.input.node", number(cardNodeId)},
		                                           {"link.input.port", "playback_AUX" + number(channel - 1)}}});
	}
	for (const Held& link : receiverLinks) {
		const std::string output = link.contains("link.output.port") ? link.at("link.output.port") : "";
		const std::uint32_t aux = output.starts_with("receive_AUX") ? std::stoul(output.substr(11)) : 99;
		instances[Home::companionLink].push_back({"a receiver link from " + output,
		                                          link,
		                                          {{"link.output.node", number(receiverNodeId)},
		                                           {"link.output.port", "receive_AUX" + number(aux)},
		                                           {"link.input.node", number(fppId)},
		                                           {"link.input.port", "from_rtp." + number(aux + 1)}}});
	}

	for (const Home home : allHomes) {
		INFO(homeName(home));
		CHECK_FALSE(instances[home].empty());
	}
	for (const PropertyRow& row : engineProperties) {
		for (const Instance& instance : instances[row.home]) {
			INFO(homeName(row.home) << " " << row.key << " on " << instance.what);
			const auto expected = instance.expected.find(row.key);
			const std::string want = expected != instance.expected.end() ? expected->second : std::string(row.value);
			const auto heldAs = instance.heldAs.find(row.key);
			const auto found = instance.props.find(heldAs != instance.heldAs.end() ? heldAs->second : row.key);
			if (instance.consumed.contains(row.key)) {
				CHECK(found == instance.props.end());
				continue;
			}
			CHECK(found != instance.props.end());
			if (found == instance.props.end()) continue;
			if (row.home == Home::rtpReceiver && row.key == "stream.props") {
				CHECK(parsed(found->second) == streamRows);
			} else {
				CHECK(found->second == want);
			}
		}
	}
	// No object holds a key `forbidden` refuses on its home, unless PipeWire itself sets it there.
	for (const Home home : allHomes) {
		for (const Instance& instance : instances[home]) {
			if (!instance.judgeForbidden) continue;
			for (const auto& [key, value] : instance.props) {
				INFO(homeName(home) << " " << key << " on " << instance.what);
				CHECK((!isForbidden(home, key) || instance.setByPipeWire.contains(key)));
			}
		}
	}

	receiver.reset();
	card.reset();
	ear::testing::silenceOrAbort(*cardBlock);
	ear::testing::silenceOrAbort(*fppBlock);
}

TEST_CASE("engine.conf sets exactly the property table's engine.conf and internal clock rows, and nothing it forbids",
          "[properties]") {
	const Held top = parsed(readFile(EAR_ENGINE_CONF));
	// libpipewire reads properties from other sections too (filter.properties, stream.properties, node.rules and
	// more): the file has these four and nothing else.
	CHECK(keysOf(top) ==
	      std::set<std::string>{"context.properties", "context.spa-libs", "context.modules", "context.objects"});
	REQUIRE(top.contains("context.properties"));
	REQUIRE(top.contains("context.objects"));
	REQUIRE(top.contains("context.modules"));

	const Held properties = parsed(top.at("context.properties"));
	CHECK(keysOf(properties) == tableKeys(Home::engineConf));
	for (const auto& [key, value] : properties) {
		INFO(key);
		CHECK_FALSE(isForbidden(Home::engineConf, key));
	}

	// A module loaded with arguments would set properties of its own: each is a name and flags alone.
	const std::vector<Held> modules = elements(top.at("context.modules"));
	CHECK_FALSE(modules.empty());
	for (const Held& module : modules) {
		INFO((module.contains("name") ? module.at("name") : std::string("a module with no name")));
		CHECK(module.contains("name"));
		for (const auto& [key, value] : module) CHECK((key == "name" || key == "flags"));
	}

	const std::vector<Held> objects = elements(top.at("context.objects"));
	REQUIRE(objects.size() == 1); // the internal clock, and nothing else
	CHECK(objects[0].at("factory") == "spa-node-factory");
	const Held clock = parsed(objects[0].at("args"));
	CHECK(keysOf(clock) == tableKeys(Home::internalClock));

	for (const PropertyRow& row : engineProperties) {
		INFO(homeName(row.home) << " " << row.key);
		if (row.home == Home::engineConf) CHECK(properties.at(std::string(row.key)) == row.value);
		if (row.home == Home::internalClock) CHECK(clock.at(std::string(row.key)) == row.value);
	}
}

TEST_CASE("Every row of the property table is pinned by a test that exists", "[properties]") {
	std::set<std::string> names;
	for (const Catch::TestCase& test : Catch::getRegistryHub().getTestCaseRegistry().getAllTests())
		names.insert(test.name);
	for (const PropertyRow& row : engineProperties) {
		INFO(homeName(row.home) << " " << row.key << ": " << row.pinnedBy);
		CHECK(names.contains(std::string(row.pinnedBy)));
	}
}

TEST_CASE("docs/engine.md lists the property table's rows and forbidden keys, each row's why under a heading that "
          "exists",
          "[properties]") {
	const Doc doc = readDoc();
	REQUIRE_FALSE(doc.setRows.empty());
	REQUIRE_FALSE(doc.neverRows.empty());

	std::set<std::pair<Home, std::string>> documented;
	for (const std::vector<std::string>& cell : doc.setRows) {
		REQUIRE(cell.size() == 6); // where, key, value, why, couples to, pinned by
		const auto home = homeNamed(plain(cell[0]));
		const std::string key = plain(cell[1]);
		INFO(cell[0] << " " << key);
		REQUIRE(home);
		CHECK(documented.emplace(*home, key).second);
		const auto row = std::ranges::find_if(engineProperties, [&](const PropertyRow& candidate) {
			return candidate.home == *home && candidate.key == key;
		});
		REQUIRE(row != engineProperties.end());
		// A value as set is one backticked span, exactly the row's; a value that says how it is computed is prose,
		// with no backticks.
		const std::string value = trim(cell[2]);
		if (value.find('`') != std::string::npos) CHECK(value == "`" + std::string(row->value) + "`");
		else CHECK(value == row->value);
		CHECK(cell[3].find("(#" + std::string(row->anchor) + ")") != std::string::npos);
		CHECK_FALSE(trim(cell[4]).empty()); // what it couples to: its home is this page
		CHECK(plain(cell[5]) == row->pinnedBy);
	}
	for (const PropertyRow& row : engineProperties) {
		INFO(homeName(row.home) << " " << row.key);
		CHECK(documented.contains({row.home, std::string(row.key)}));
		CHECK(doc.anchors.contains(std::string(row.anchor)));
	}

	std::set<std::pair<Home, std::string>> never;
	for (const std::vector<std::string>& cell : doc.neverRows) {
		REQUIRE(cell.size() == 3); // where, keys, why
		for (const std::string& where : splitList(cell[0])) {
			for (std::string key : splitList(cell[1])) {
				if (key.ends_with(".*")) key.pop_back(); // a prefix: "default.clock.*" is "default.clock."
				INFO(where << " " << key);
				const auto home = homeNamed(where);
				REQUIRE(home);
				CHECK(never.emplace(*home, key).second);
			}
		}
	}
	std::set<std::pair<Home, std::string>> refused;
	for (const auto& [home, key] : forbidden) refused.emplace(home, std::string(key));
	CHECK(never == refused);
}

TEST_CASE("A module argument's value is one value: whitespace outside quotes, an array or an object would open another "
          "key, and only stream.props nests the RTP stream's arguments",
          "[properties]") {
	ModuleArguments<Home::rtpReceiver> arguments;
	const PropertyKey<Home::rtpReceiver> name = property<Home::rtpReceiver>("node.name");
	for (const char* value : {"true", "\"fpp rtp\"", "[ AUX0 AUX1 ]", "{ mode = dsp monitor = false }", "48000"}) {
		INFO(value);
		CHECK_NOTHROW(arguments.add(name, value));
	}
	for (const char* value : {"true remote.name = \"elsewhere\"", "", "[ AUX0", "\"open", "] x", "{ a = b } c = d"}) {
		INFO(value);
		CHECK_THROWS_AS(arguments.add(name, value), std::logic_error);
	}
	ModuleArguments<Home::rtpStream> stream;
	stream.add(property<Home::rtpStream>("node.always-process"), "true");
	CHECK_THROWS_AS(arguments.add(name, stream), std::logic_error);
	CHECK_NOTHROW(arguments.add(property<Home::rtpReceiver>("stream.props"), stream));
	CHECK(arguments.text().ends_with(" stream.props = { node.always-process = true } }"));
}
