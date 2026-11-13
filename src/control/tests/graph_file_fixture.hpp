// The typed values behind test/protocol/graph-file.json (docs/control-port.md#the-graph-file-over-the-port) and
// snapshot.json: the session's texts, its graph files and its graphs, and a node's health as NodeHost tells it, shared
// by the encoder tests (graph_file_state_test.cpp, commands_test.cpp) and the session replay (session_test.cpp), so
// both build the fixture's messages from one set of values.
#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "catch2/catch.hpp"
#include "ear/control/graph_file_state.hpp"
#include "ear/core/graph_view.hpp"

namespace ear::control::testing {

/// 2026-11-09T10:00:00Z: where the session replay's gateway clock starts (it moves 10 ms at each reading).
inline constexpr std::chrono::system_clock::time_point testEpoch{std::chrono::seconds{1'794'218'400}};
/// The instant `n` readings of that clock after its start.
inline std::chrono::system_clock::time_point reading(int n) { return testEpoch + std::chrono::milliseconds(10 * n); }

/// A health as NodeHost tells it, by its state: starting is the configuring phase, stopped is wanted stopped, disabled
/// is wanted disabled; every other state is its own phase, wanted running.
inline ear::NodeHealth health(const std::string& node, ear::NodeState state,
                              std::optional<ear::FailureReason> reason = {}) {
	ear::NodeHealth h{node, state, std::move(reason)};
	switch (state) {
	case ear::NodeState::starting:
		h.phase = ear::NodePhase::configuring;
		break;
	case ear::NodeState::running:
		h.phase = ear::NodePhase::running;
		break;
	case ear::NodeState::stopping:
		h.phase = ear::NodePhase::stopping;
		break;
	case ear::NodeState::stopped:
		h.phase = ear::NodePhase::stopped;
		h.wanted = ear::Wanted::stopped;
		break;
	case ear::NodeState::disabled:
		h.phase = ear::NodePhase::disabled;
		h.wanted = ear::Wanted::disabled;
		break;
	case ear::NodeState::failed:
		h.phase = ear::NodePhase::failed;
		break;
	}
	return h;
}

/// The record's file format example (docs/graph-file.md#edits-at-the-source).
inline const std::string recordExample = R"(graph_frequency 48000;
block_size 256;
@set $ch 2;                      # comments survive every edit
@snippet card_out $id {          # written once, used by both cards
    card $id;
    playback $ch;
}
node white noise;
node file rec { path "take1.wav"; }
node card desk { @use card_out MOTU; }
node card porch { @use card_out USB2; }
route noise:noise.mono rec:record.left;
route noise:noise.mono desk:play.1;
)";

inline std::string replaced(std::string text, std::string_view from, std::string_view to) {
	const auto at = text.find(from);
	REQUIRE(at != std::string::npos);
	return text.replace(at, from.size(), to);
}

/// The texts of the session in graph-file.json, in order.
struct Texts {
	std::string t1 = recordExample;
	std::string t2 = replaced(t1, "take1.wav", "take2.wav");
	std::string t3 = t2 + "@include \"more.conf\";\n";
	std::string t5 = replaced(t2, "take2.wav", "take3.wav");
	std::string t7 = replaced(t2, "@set $ch 2;", "@set $ch 4;");
	std::string t8 = replaced(t7, "take2.wav", "take3.wav");
	std::string t9 = t7 + "route noise:noise.mono rec:FX;\n";
	std::string t10 = replaced(t7, "graph_frequency 48000;", "graph_frequency 44100;");
	std::string t11 = replaced(t10, "take2.wav", "take4.wav");
};

/// A file with no diagnostics, refusal, pending restart or write failure.
inline GraphFile file(std::string version, std::string text, std::optional<std::string> running) {
	return GraphFile{std::move(version), std::move(text), {}, std::move(running), std::nullopt, {}, std::nullopt};
}

inline const Diagnostic includeUnsupported{Severity::error, "ear/include-unsupported",
                                           "@include is not supported yet: write the whole graph in ear.conf",
                                           TextPlace{14, 1}};

/// graph-file.json's graph: the cards fail (no such card), noise and rec run.
inline ear::GraphView graphView(int playback, const std::string& recPath) {
	using ear::NodeState;
	using Code = ear::FailureReason::Code;
	ear::GraphView view{{{48000, 256}, false, "SCHED_OTHER"}, {"card", "file", "white"}, {}, {}};
	const ear::Ports cardPorts{{{ear::Direction::input, "play", ear::numbered(static_cast<std::uint32_t>(playback))}}};
	view.nodes.push_back(
		{{"desk", "card", {{"card", "MOTU"}, {"playback", playback}}},
	     cardPorts,
	     health("desk", NodeState::failed,
	            ear::FailureReason{Code::engineRefused, "cannot open hw:CARD=MOTU,DEV=0: No such device"})});
	view.nodes.push_back({{"noise", "white", nlohmann::json::object()},
	                      ear::Ports{{{ear::Direction::output, "noise", {"mono"}}}},
	                      health("noise", NodeState::running)});
	view.nodes.push_back(
		{{"porch", "card", {{"card", "USB2"}, {"playback", playback}}},
	     cardPorts,
	     health("porch", NodeState::failed,
	            ear::FailureReason{Code::engineRefused, "cannot open hw:CARD=USB2,DEV=0: No such device"})});
	view.nodes.push_back({{"rec", "file", {{"path", recPath}}},
	                      ear::Ports{{{ear::Direction::input, "record", {"left", "right"}}}},
	                      health("rec", NodeState::running)});
	view.routes.push_back({{{"noise", "noise.mono"}, {"desk", "play.1"}}, ear::RouteMatch::pending, ""});
	view.routes.push_back({{{"noise", "noise.mono"}, {"rec", "record.left"}}, ear::RouteMatch::linked, ""});
	return view;
}

/// snapshot.json's graph, as GraphStatus reads it and the gateway fills it: a card that failed, a running node the
/// gateway was told of (its since), a node ear.conf disables, a running node that missed cycles and overran, a file
/// node that failed, and the graph's counters.
inline ear::GraphView snapshotView() {
	using ear::NodeState;
	using Code = ear::FailureReason::Code;
	ear::GraphView view{{{48000, 256}, true, "SCHED_FIFO 88"}, {"card", "file", "white"}, {}, {}};
	view.nodes.push_back(
		{{"motu", "card", {{"card", "Pro24"}, {"playback", 2}}},
	     ear::Ports{{{ear::Direction::input, "play", {"1", "2"}}}},
	     health("motu", NodeState::failed,
	            ear::FailureReason{Code::engineRefused, "cannot open hw:CARD=Pro24,DEV=0: No such device"})});
	view.nodes.push_back({{"noise", "white", nlohmann::json::object()},
	                      ear::Ports{{{ear::Direction::output, "noise", {"mono"}}}},
	                      health("noise", NodeState::running),
	                      reading(0)});
	ear::NodeSpec quiet{"quiet", "white", nlohmann::json::object()};
	quiet.disabled = true;
	view.nodes.push_back(
		{quiet, ear::Ports{{{ear::Direction::output, "noise", {"mono"}}}}, health("quiet", NodeState::disabled)});
	view.nodes.push_back({{"rec", "file", {{"path", "x.wav"}}},
	                      ear::Ports{{{ear::Direction::input, "record", {"left", "right"}}}},
	                      health("rec", NodeState::running),
	                      std::nullopt,
	                      ear::NodeCounts{.missedCycles = 33, .overruns = 2}});
	view.nodes.push_back(
		{{"tape", "file", {{"path", "nope/t.wav"}}},
	     ear::Ports{{{ear::Direction::input, "record", {"left", "right"}}}},
	     health("tape", NodeState::failed,
	            ear::FailureReason{Code::startFailed, "cannot open nope/t.wav: No such file or directory"})});
	const ear::Route noiseToRecFl{{"noise", "noise.mono"}, {"rec", "record.left"}};
	view.routes.push_back({{{"noise", "noise.mono"}, {"motu", "play.1"}}, ear::RouteMatch::pending, ""});
	view.routes.push_back({noiseToRecFl, ear::RouteMatch::linked, ""});
	view.routes.push_back({{{"noise", "noise.mono"}, {"tape", "record.left"}},
	                       ear::RouteMatch::refused,
	                       "the engine removed the link before it was active"});
	view.counts = ear::GraphCounts{.cycles = 18750, .cutCycles = 39, .stalls = 1}; // lostMs 208: 39 blocks of 256
	return view;
}

} // namespace ear::control::testing
