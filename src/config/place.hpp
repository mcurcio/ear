// Places in ear.conf (docs/design/knobly-config.md, "Wire additions"): knobly's units to the wire's.
#pragma once
#include <cstdint>
#include <string_view>
#include "ear/control/graph_file_state.hpp"
namespace ear::config {
/// The one conversion from knobly's units (line and column from 0, column in UTF-8 bytes) to the
/// wire's, against ear.conf's text. A byte column inside a character counts that character; a line
/// past the end is clamped to the last line; a column past a line's end is clamped to one past its
/// last character. Pure.
control::TextPlace placeIn(std::string_view text, std::uint32_t line0, std::uint32_t byteColumn0);
}
