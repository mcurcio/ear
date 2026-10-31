// The typed values behind test/protocol/graph-file.json (docs/design/knobly-config.md, "Wire
// additions"): the session's texts, its graph files and its graph, shared by the encoder test
// (graph_file_state_test.cpp) and the session replay (session_test.cpp), so both build the fixture's
// messages from one set of values.
#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "catch2/catch.hpp"
#include "ear/control/graph_file_state.hpp"
#include "ear/core/graph_view.hpp"

namespace ear::control::testing {

/// The record's file format example (docs/design/knobly-config.md, "File format").
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
route noise:MONO rec:FL;
route noise:MONO desk:playback_1;
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
	std::string t9 = t7 + "route noise:MONO rec:FX;\n";
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
	ear::Ports cardPorts;
	for (int i = 1; i <= playback; ++i) cardPorts.inputs.push_back("playback_" + std::to_string(i));
	view.nodes.push_back({{"desk", "card", {{"card", "MOTU"}, {"playback", playback}}},
	                      cardPorts,
	                      NodeState::failed,
	                      ear::FailureReason{Code::engineRefused, "cannot open hw:CARD=MOTU,DEV=0: No such device"}});
	view.nodes.push_back({{"noise", "white", nlohmann::json::object()}, {{}, {"MONO"}}, NodeState::running, std::nullopt});
	view.nodes.push_back({{"porch", "card", {{"card", "USB2"}, {"playback", playback}}},
	                      cardPorts,
	                      NodeState::failed,
	                      ear::FailureReason{Code::engineRefused, "cannot open hw:CARD=USB2,DEV=0: No such device"}});
	view.nodes.push_back({{"rec", "file", {{"path", recPath}}}, {{"FL", "FR"}, {}}, NodeState::running, std::nullopt});
	view.routes.push_back({{{"noise", "MONO"}, {"desk", "playback_1"}}, ear::RouteMatch::pending, ""});
	view.routes.push_back({{{"noise", "MONO"}, {"rec", "FL"}}, ear::RouteMatch::linked, ""});
	return view;
}

}  // namespace ear::control::testing
