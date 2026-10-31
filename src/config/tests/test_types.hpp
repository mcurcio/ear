// Node types for ear_config's tests, shaped like the built-in ones and declaring their settings'
// kinds: "white" (output MONO, no settings), "file" (inputs FL and FR; path: text) and "card"
// (card: text; playback and capture: whole numbers, inputs playback_1..playback_N).
#pragma once
#include <memory>
#include <vector>
#include "ear/core/node_catalogue.hpp"
namespace ear::config::test {
std::vector<std::unique_ptr<const NodeType>> testTypes();
}
