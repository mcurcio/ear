// The node types EAR knows, looked up by name (docs/architecture.md#packages,
// node_catalogue.hpp).
#include "ear/core/node_catalogue.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace ear {
namespace {

/// 1 or more of [a-z0-9]: a store's file, "<type>-<node>.<suffix>", is then one type's and one node's alone
/// (docs/writing-a-node-type.md#layout-and-registration).
bool validTypeName(std::string_view name) {
	return !name.empty() &&
	       std::ranges::all_of(name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

auto named(std::string_view name) {
	return [name](const std::unique_ptr<const NodeType>& type) { return type->name() == name; };
}

} // namespace

NodeCatalogue::NodeCatalogue(std::vector<std::unique_ptr<const NodeType>> types) : types_(std::move(types)) {
	for (auto it = types_.begin(); it != types_.end(); ++it) {
		if (!*it) throw std::invalid_argument("node catalogue: a node type is null");
		if (!validTypeName((*it)->name()))
			throw std::invalid_argument("node catalogue: a node type's name is 1 or more of [a-z0-9], not \"" +
			                            std::string((*it)->name()) + "\"");
		if (std::any_of(types_.begin(), it, named((*it)->name())))
			throw std::invalid_argument("node catalogue: two node types are named " + std::string((*it)->name()));
	}
}

const NodeType* NodeCatalogue::find(std::string_view name) const noexcept {
	const auto it = std::find_if(types_.begin(), types_.end(), named(name));
	return it == types_.end() ? nullptr : it->get();
}

std::vector<std::string_view> NodeCatalogue::names() const {
	std::vector<std::string_view> all;
	all.reserve(types_.size());
	for (const auto& type : types_) all.push_back(type->name());
	std::sort(all.begin(), all.end());
	return all;
}

} // namespace ear
