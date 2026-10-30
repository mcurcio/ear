// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in that page's Errata section and a re-cut, never a local edit.
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
private:
  std::vector<std::unique_ptr<const NodeType>> types_;
};
}
