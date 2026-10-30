// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <memory>
#include <string_view>
#include <vector>
#include "ear/core/node_type.hpp"
namespace ear {
class NodeCatalogue {
public:
  /// Throws std::invalid_argument if two types share a name (boot-time check).
  explicit NodeCatalogue(std::vector<std::unique_ptr<const NodeType>> types);
  const NodeType* find(std::string_view name) const noexcept;   // nullptr: unknown
  std::vector<std::string_view> names() const;                  // every type's name, sorted
private:
  std::vector<std::unique_ptr<const NodeType>> types_;
};
}
