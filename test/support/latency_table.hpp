// The latency model's table (header-only; the target ear_latency_table puts it on the include path): one row per bank
// shape and per past regression, with what workOut answers for it (docs/engine.md#latency-per-bank-pair).
// ear_core_tests runs every row through workOut (src/core/tests/latency_test.cpp); ear_pipewire_tests runs the same
// rows through a real port block, its peers real blocks linked to it (the conformance test in
// src/pipewire/tests/port_block_test.cpp), so the block and the model cannot drift apart.
#pragma once

#include <optional>
#include <vector>

#include "ear/core/latency.hpp"
#include "ear/core/node_type.hpp"

namespace ear::test {

/// One bank shape: the banks, the plan, what each port's peer reports (none: no peer), and what workOut answers.
struct LatencyRow {
	const char* what;
	std::vector<PortBank> banks;
	LatencyPlan plan;
	std::vector<std::optional<Latency>> inputPeers;  // one per input port, inputs then insideInputs
	std::vector<std::optional<Latency>> outputPeers; // one per output port, outputs then insideOutputs
	std::vector<Latency> told;                       // workOut's told: inputs then insideInputs, then the outputs
	std::vector<Latency> downstream;                 // workOut's downstream, one per output port
};

inline std::vector<LatencyRow> latencyRows() {
	using R = PortBank::Reach;
	const std::optional<Latency> none;
	return {
		{"an input bank in no pair tells its own delay alone, whatever its peers",
	     {{Direction::input, "in", numbered(2)}},
	     {.onBank = {{"in", Delay{{7, 9}}}}},
	     {Latency{10, 20}, Latency{30, 40}},
	     {},
	     {{7, 9}, {7, 9}},
	     {}},
		{"an output bank in no pair tells zero; its downstream is its peers side by side",
	     {{Direction::output, "out", numbered(2)}},
	     {},
	     {},
	     {Latency{5, 7}, Latency{3, 9}},
	     {{0, 0}, {0, 0}},
	     {{3, 9}, {3, 9}}},
		{"an output bank with no peer has zero downstream, and tells its own delay",
	     {{Direction::output, "out", numbered(1)}},
	     {.onBank = {{"out", Delay{{32, 32}}}}},
	     {},
	     {none},
	     {{32, 32}},
	     {{0, 0}}},
		{"a pair carries each side's beyond to the other",
	     {{Direction::input, "in", numbered(2)}, {Direction::output, "out", numbered(2)}},
	     {.pairs = {{"in", "out"}}},
	     {Latency{10, 20}, Latency{12, 18}},
	     {Latency{100, 200}, none},
	     {{100, 200}, {100, 200}, {10, 20}, {10, 20}},
	     {{100, 200}, {100, 200}}},
		// Regression (no path vs zero): the output port with no peer is no path, so the input tells the buffer whole.
		{"a port with no peer is no path",
	     {{Direction::input, "in", numbered(2)}, {Direction::output, "out", numbered(2)}},
	     {.pairs = {{"in", "out"}}},
	     {none, none},
	     {none, Latency{300, 300}},
	     {{300, 300}, {300, 300}, {0, 0}, {0, 0}},
	     {{300, 300}, {300, 300}}},
		{"a port whose peer reports zero is a zero path",
	     {{Direction::input, "in", numbered(2)}, {Direction::output, "out", numbered(2)}},
	     {.pairs = {{"in", "out"}}},
	     {none, none},
	     {Latency{0, 0}, Latency{300, 300}},
	     {{0, 300}, {0, 300}, {0, 0}, {0, 0}},
	     {{0, 300}, {0, 300}}},
		// Regression (parallel peers' min/max), on a bank.
		{"parallel peers on one bank",
	     {{Direction::input, "in", numbered(1)}, {Direction::output, "out", numbered(3)}},
	     {.pairs = {{"in", "out"}}},
	     {none},
	     {Latency{64, 64}, Latency{256, 256}, Latency{128, 512}},
	     {{64, 512}, {0, 0}, {0, 0}, {0, 0}},
	     {{64, 512}, {64, 512}, {64, 512}}},
		// Two pairs into one output are two paths side by side; a paired bank with no peer is a zero path (PipeWire
		// finishes an empty combination at zero, and the pair still carries it).
		{"two pairs into one output",
	     {{Direction::input, "a", numbered(1)},
	      {Direction::input, "b", numbered(1)},
	      {Direction::output, "out", numbered(1)}},
	     {.pairs = {{"a", "out"}, {"b", "out"}}},
	     {Latency{100, 200}, none},
	     {none},
	     {{0, 0}, {0, 0}, {0, 200}},
	     {{0, 0}}},
		// Regression (3b.2, 6c1299b556): a code's delay declared on its pair reaches the input bank's told playback
		// latency, so a source on it tells its clock enough playback latency; and the output's capture likewise.
		{"a pair's delay counts both ways on its path",
	     {{Direction::output, "out", numbered(1)}, {Direction::input, "src", numbered(1), R::inside}},
	     {.pairs = {{"src", "out"}},
	      .onPair = {{{"src", "out"}, Delay{{0, 64}}}},
	      .beyond = {{"src", Delay{{0, 1024}}}}},
	     {none},
	     {Latency{256, 256}},
	     {{256, 320}, {0, 1088}},
	     {{256, 256}}},
		// Regression (the class of 3b.2): a type's own code declares its delay from `in` to `out` and names no
		// pass-through; NodeHost carries the declared path as a pair, so the delay reaches `in`'s told playback
		// latency.
		{"a type's own code's declared path carries its delay both ways",
	     {{Direction::input, "in", numbered(1)}, {Direction::output, "out", numbered(1)}},
	     {.pairs = {{"in", "out"}}, .onPair = {{{"in", "out"}, Delay{{0, 480}}}}},
	     {Latency{10, 10}},
	     {Latency{256, 256}},
	     {{256, 736}, {10, 490}},
	     {{256, 256}}},
		// A mixer: a code of the type's own with two inputs into one output declares each path's delay; each is
		// carried both ways, and the output tells both paths side by side.
		{"a mixer's two declared paths into one output, each delay on its own path",
	     {{Direction::input, "a", numbered(1)},
	      {Direction::input, "b", numbered(1)},
	      {Direction::output, "mix", numbered(1)}},
	     {.pairs = {{"a", "mix"}, {"b", "mix"}},
	      .onPair = {{{"a", "mix"}, Delay{{64, 64}}}, {{"b", "mix"}, Delay{{256, 512}}}}},
	     {Latency{10, 10}, Latency{100, 100}},
	     {Latency{48, 48}},
	     {{112, 112}, {304, 560}, {74, 612}},
	     {{48, 48}}},
		{"a pair's delay is on its path only, not on another pair's into the same output",
	     {{Direction::input, "a", numbered(1)},
	      {Direction::input, "b", numbered(1)},
	      {Direction::output, "out", numbered(1)}},
	     {.pairs = {{"a", "out"}, {"b", "out"}}, .onPair = {{{"a", "out"}, Delay{{50, 50}}}}},
	     {Latency{10, 10}, Latency{20, 20}},
	     {Latency{5, 5}},
	     {{55, 55}, {5, 5}, {20, 60}},
	     {{5, 5}}},
		// sendspin's feed, declared on its unpaired output `from`.
		{"a bank's own delay is on what it tells, not on its downstream",
	     {{Direction::input, "to", numbered(2)}, {Direction::output, "from", numbered(2)}},
	     {.onBank = {{"from", Delay{{0, 9600}}}}},
	     {none, none},
	     {Latency{256, 256}, Latency{256, 256}},
	     {{0, 0}, {0, 0}, {0, 9600}, {0, 9600}},
	     {{256, 256}, {256, 256}}},
		{"an input's own delay is on what it tells only",
	     {{Direction::input, "in", numbered(1)}, {Direction::output, "out", numbered(1)}},
	     {.pairs = {{"in", "out"}}, .onBank = {{"in", Delay{{7, 7}}}}},
	     {Latency{10, 10}},
	     {Latency{100, 100}},
	     {{107, 107}, {10, 10}},
	     {{100, 100}}},
		// Regression (3b.3, 40be60af04): rtp-source's ports report zero once linked; the receiver's buffer lies behind
		// them, in series, so `stream` tells {B, B}, never {0, B}.
		{"a companion's beyond in series with peers reporting zero",
	     {{Direction::output, "stream", numbered(2)}, {Direction::input, "from_rtp", numbered(2), R::inside}},
	     {.pairs = {{"from_rtp", "stream"}}, .beyond = {{"from_rtp", Delay{{480, 480}}}}},
	     {Latency{0, 0}, Latency{0, 0}},
	     {none, none},
	     {{0, 0}, {0, 0}, {480, 480}, {480, 480}},
	     {{0, 0}, {0, 0}}},
		{"a companion's beyond with no peer at all",
	     {{Direction::output, "stream", numbered(1)}, {Direction::input, "from_rtp", numbered(1), R::inside}},
	     {.pairs = {{"from_rtp", "stream"}}, .beyond = {{"from_rtp", Delay{{480, 480}}}}},
	     {none},
	     {none},
	     {{0, 0}, {480, 480}},
	     {{0, 0}}},
		{"a beyond on an output bank lies in series with its downstream",
	     {{Direction::input, "in", numbered(1)}, {Direction::output, "out", numbered(1)}},
	     {.pairs = {{"in", "out"}}, .beyond = {{"out", Delay{{16, 16}}}}},
	     {none},
	     {Latency{100, 200}},
	     {{116, 216}, {0, 0}},
	     {{116, 216}}},
		// A source's ring declared beyond its bank: [0, capacity].
		{"a source's ring",
	     {{Direction::output, "out", numbered(2)}, {Direction::input, "ring", numbered(2), R::inside}},
	     {.pairs = {{"ring", "out"}}, .beyond = {{"ring", Delay{{0, 1024}}}}},
	     {none, none},
	     {none, none},
	     {{0, 0}, {0, 0}, {0, 1024}, {0, 1024}},
	     {{0, 0}, {0, 0}}},
		// A card block: playback from `play` to the card and capture from the card to `capture`, never mixed.
		{"a card block's two directions kept apart",
	     {{Direction::input, "play", numbered(1)},
	      {Direction::output, "capture", numbered(1)},
	      {Direction::output, "to_card", numbered(1), R::inside},
	      {Direction::input, "from_card", numbered(1), R::inside}},
	     {.pairs = {{"play", "to_card"}, {"from_card", "capture"}}},
	     {Latency{10, 10}, Latency{128, 128}},
	     {Latency{20, 20}, Latency{256, 256}},
	     {{256, 256}, {20, 20}, {128, 128}, {10, 10}},
	     {{20, 20}, {256, 256}}},
	};
}

} // namespace ear::test
