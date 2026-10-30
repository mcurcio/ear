// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in that page's Errata section and a re-cut, never a local edit.
#pragma once
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include "ear/core/desired_graph.hpp"
namespace ear::control {
struct GraphFileError { std::string detail; };   // names file and JSON pointer, e.g. "/routes/0/to/port: missing"
/// Pure. {"graphFrequency": uint>0, "blockSize": uint>0 (both required, no defaults),
///  "nodes": [{"name","type","settings"?}], "routes": [{"from":{"node","port"},"to":{...}}]}.
/// Unknown keys are errors. Node settings are passed through unchecked.
std::expected<GraphSpec, GraphFileError> parseGraphFile(std::string_view json);
std::expected<GraphSpec, GraphFileError> readGraphFile(const std::filesystem::path& path);
/// Boot: nodes in file order, then routes in file order; stops at the first error.
std::expected<void, CommandError> replay(const GraphSpec& spec, GraphControl& control);
}
