// Every node type together, however many the folders in src/nodes make: their names, and each type's declared setting
// kinds (docs/graph-file.md). That main.cpp registers them is the system tests' to prove.
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "all_types.hpp"
#include "catch2/catch.hpp"
#include "ear/core/node_catalogue.hpp"

// Each found; the whole list is the next test's to prove against the folders, and sendspin is the last test's.
TEST_CASE("The built-in node types are white, file, card and rtp", "[builtin]") {
	const ear::NodeCatalogue catalogue(ear::test::allTypes());
	for (const std::string_view name : {"white", "file", "card", "rtp"}) {
		INFO(name);
		CHECK(catalogue.find(name) != nullptr);
	}
}

TEST_CASE("Every folder in src/nodes makes one type of its own name", "[nodes]") {
	const ear::NodeCatalogue catalogue(ear::test::allTypes());
	std::string joined;
	for (const std::string_view name : catalogue.names()) {
		if (!joined.empty()) joined += ',';
		joined += name;
	}
	CHECK(joined == EAR_NODE_TYPE_FOLDERS);
}

TEST_CASE("Each built-in type declares every key once, and a value of each declared kind parses", "[builtin]") {
	std::map<std::string, std::vector<std::pair<std::string, ear::SettingKind>>> declared;
	for (const auto& type : ear::test::allTypes()) {
		INFO(type->name());
		const auto& valid = ear::test::validSettings(type->name());
		for (const ear::NodeSettings& settings : valid) {
			const auto parsed = type->parse("node", settings);
			INFO(settings.dump() << " is refused: " << (parsed ? "" : parsed.error().detail));
			CHECK(parsed.has_value());
		}
		auto& keys = declared[std::string(type->name())];
		for (const auto& [key, kind] : type->settingKinds()) {
			INFO(key);
			const std::string spelled(key);
			CHECK(std::ranges::none_of(keys, [&](const auto& seen) { return seen.first == spelled; }));
			const auto giving = std::ranges::find_if(valid, [&](const auto& one) { return one.contains(spelled); });
			REQUIRE(giving != valid.end());
			CHECK((kind == ear::SettingKind::text) == giving->at(spelled).is_string());
			keys.emplace_back(key, kind);
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
	                                                                               {"rate", rate},
	                                                                               {"latencyMs", wholeNumber}});
	CHECK(declared["sendspin"] == std::vector<std::pair<std::string, ear::SettingKind>>{
									  {"server", text}, {"port", wholeNumber}, {"name", text}});
}
