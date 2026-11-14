// Node types for ear_config's tests, shaped like the built-in ones and declaring their settings'
// kinds: "white" (output noise.mono, no settings), "file" (inputs record.left and record.right; path: text) and "card"
// (card: text; playback and capture: whole numbers, inputs play.1..play.N) and "rtp" (rate: a rate, 8khz to 192khz;
// output stream.1).
#pragma once
#include <memory>
#include <vector>
#include "ear/core/node_catalogue.hpp"
namespace ear::config::test {
std::vector<std::unique_ptr<const NodeType>> testTypes();
}
