// Every node type together: the five names, and each type's declared setting kinds
// (docs/design/knobly-config.md). That main.cpp registers all five is the system tests' to prove.
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/node_catalogue.hpp"
#include "ear/nodes/card.hpp"
#include "ear/nodes/file.hpp"
#include "ear/nodes/rtp.hpp"
#include "ear/nodes/sendspin.hpp"
#include "ear/nodes/white.hpp"

namespace {

/// Both folders are nullptr: parse never opens them (src/nodes/sendspin/tests/settings_test.cpp does the same), and
/// these tests start no node.
std::vector<std::unique_ptr<const ear::NodeType>> nodeTypes() {
	std::vector<std::unique_ptr<const ear::NodeType>> types;
	types.push_back(ear::nodes::makeWhiteNodeType());
	types.push_back(ear::nodes::makeFileNodeType(nullptr));
	types.push_back(ear::nodes::makeCardNodeType());
	types.push_back(ear::nodes::makeRtpNodeType());
	types.push_back(ear::nodes::makeSendspinNodeType(nullptr));
	return types;
}

} // namespace

TEST_CASE("The node types are card, file, rtp, sendspin and white", "[nodes]") {
	const ear::NodeCatalogue catalogue(nodeTypes());
	CHECK(catalogue.names() == std::vector<std::string_view>{"card", "file", "rtp", "sendspin", "white"});
}

TEST_CASE("Each built-in type declares every key once, and a value of each declared kind parses", "[nodes]") {
	std::map<std::string, std::vector<std::pair<std::string, ear::SettingKind>>> declared;
	for (const auto& type : nodeTypes()) {
		INFO(type->name());
		// "a" is a valid card id and a valid recordings path; 1 is a valid channel count, port and
		// latency. The rtp keys whose values those are not get a valid value of the same kind.
		const std::map<std::string, ear::NodeSettings> valid{
			{"address", "0.0.0.0"}, {"format", "L16"}, {"rate", 48000}};
		ear::NodeSettings settings = ear::NodeSettings::object();
		auto& keys = declared[std::string(type->name())];
		for (const auto& [key, kind] : type->settingKinds()) {
			CHECK_FALSE(settings.contains(std::string(key)));
			const auto special = valid.find(std::string(key));
			settings[std::string(key)] = special != valid.end()           ? special->second
			                             : kind == ear::SettingKind::text ? ear::NodeSettings("a")
			                                                              : ear::NodeSettings(1);
			CHECK((kind == ear::SettingKind::text) == settings[std::string(key)].is_string());
			keys.emplace_back(key, kind);
		}
		// sendspin refuses its keys all together ("port" only without "server"), and "a" is no
		// server URL, so its two ways to be set are parsed below instead.
		if (type->name() == "sendspin") {
			CHECK(type->parse("node", {{"port", 1}, {"name", "a"}}).has_value());
			CHECK(type->parse("node", {{"server", "ws://a/b"}, {"name", "a"}}).has_value());
		} else {
			CHECK(type->parse("node", settings).has_value());
		}
	}
	using enum ear::SettingKind;
	CHECK(declared["white"].empty());
	CHECK(declared["file"] == std::vector<std::pair<std::string, ear::SettingKind>>{{"path", text}});
	CHECK(declared["card"] == std::vector<std::pair<std::string, ear::SettingKind>>{
								  {"card", text}, {"playback", wholeNumber}, {"capture", wholeNumber}});
	CHECK(declared["rtp"] == std::vector<std::pair<std::string, ear::SettingKind>>{{"address", text},
	                                                                               {"port", wholeNumber},
	                                                                               {"format", text},
	                                                                               {"channels", wholeNumber},
	                                                                               {"rate", wholeNumber},
	                                                                               {"latencyMs", wholeNumber}});
	CHECK(declared["sendspin"] == std::vector<std::pair<std::string, ear::SettingKind>>{
									  {"server", text}, {"port", wholeNumber}, {"name", text}});
}
