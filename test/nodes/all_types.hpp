// The folders the all-type tests hand the node types: real and empty, beneath one temporary folder that lasts the test
// program. These tests parse settings and start no node, so nothing is written in them.
#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ear/folder/confined_folder.hpp"
#include "temp_folder.hpp"

namespace ear::test {

/// `name`, made and opened beneath the temporary folder. Throws when it cannot be: Catch reports the test's failure.
inline std::shared_ptr<const folder::ConfinedFolder> openedForAllTypes(std::string_view name) {
	static const TempFolder root("all-types");
	auto opened = folder::ConfinedFolder::open(root.path() / name);
	if (!opened) throw std::runtime_error("cannot open " + std::string(name) + ": " + opened.error().detail);
	return std::make_shared<const folder::ConfinedFolder>(std::move(*opened));
}

inline const std::shared_ptr<const folder::ConfinedFolder>& recordingsFolder() {
	static const auto folder = openedForAllTypes("recordings");
	return folder;
}

inline const std::shared_ptr<const folder::ConfinedFolder>& configFolder() {
	static const auto folder = openedForAllTypes("config");
	return folder;
}

} // namespace ear::test
