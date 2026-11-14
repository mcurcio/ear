// What a declared store is to the framework (docs/writing-a-node-type.md#stores): its rule and its suffix, apart from
// the rest of store.hpp so NodeType (node_type.hpp) can answer its stores without the type author's API. Pure: nothing
// but the standard library.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
namespace ear {
/// What a store's file may be (docs/writing-a-node-type.md#stores), fixed at compile time by store::file.
struct StoreRule {
	std::uint32_t mode = 0600;          // the most it may be: a wider file is narrowed, never refused
	std::uint32_t maxBytes = 0;         // required: a larger file is neither read nor written
	std::string (*initial)() = nullptr; // the text a missing file is created with; null: a missing file stays missing
	/// Why a file's text is not trusted (the node fails, the file left as it was), or nothing; null: any text.
	std::optional<std::string> (*distrust)(std::string_view text) = nullptr;
};

/// One declared store, as the framework reads it.
struct StoreDeclaration {
	std::string_view suffix;
	StoreRule rule;
};
} // namespace ear
