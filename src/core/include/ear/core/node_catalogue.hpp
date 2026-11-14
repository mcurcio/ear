#pragma once
#include <memory>
#include <string_view>
#include <vector>
#include "ear/core/node_type.hpp"
namespace ear {
class NodeCatalogue {
public:
	/// Throws std::invalid_argument if two types share a name, or a name is not 1 or more of [a-z0-9] (boot-time
	/// checks; a store's file, "<type>-<node>.<suffix>", is then one type's and one node's alone).
	explicit NodeCatalogue(std::vector<std::unique_ptr<const NodeType>> types);
	const NodeType* find(std::string_view name) const noexcept; // nullptr: unknown
	std::vector<std::string_view> names() const;                // every type's name, sorted
private:
	std::vector<std::unique_ptr<const NodeType>> types_;
};
} // namespace ear
