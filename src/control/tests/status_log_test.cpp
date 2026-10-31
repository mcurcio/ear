// The status lines (docs/design/m1-white-to-file.md section 2; docs/design/knobly-config.md, S3
// criterion 10): each graph file change prints its first line, then exactly the lines that apply, in
// order; each write prints one line.
#include <sstream>
#include <string>

#include "catch2/catch.hpp"
#include "ear/control/status_log.hpp"

using namespace ear::control;

namespace {

GraphFile file(std::string version, std::optional<std::string> running) {
	return GraphFile{std::move(version), "text", {}, std::move(running), std::nullopt, {}, std::nullopt};
}

std::string printed(const GraphFileChanged& change) {
	std::ostringstream out;
	StatusLog log(out);
	log.graphFileChanged(change);
	return out.str();
}

} // namespace

TEST_CASE("An edit with nothing wrong prints its first line only", "[status_log]") {
	CHECK(printed({file("v2", "v2"), FileChangeCause::edit, 0}) == "graph-file edit v2 running=v2\n");
	CHECK(printed({file("v1", std::nullopt), FileChangeCause::write, 0}) == "graph-file write v1 running=none\n");
}

TEST_CASE("Each error diagnostic is a line; warnings are not", "[status_log]") {
	GraphFile broken = file("v3", "v2");
	broken.diagnostics = {
		Diagnostic{Severity::error, "E2001", "unexpected token: \"}\"", TextPlace{9, 34}},
		Diagnostic{Severity::warning, "W3005", "nothing matched", TextPlace{10, 1}},
		Diagnostic{Severity::error, "ear/include-unsupported", "@include is not supported yet: write the whole graph in ear.conf",
		           TextPlace{14, 1}},
	};
	CHECK(printed({broken, FileChangeCause::disk, 0}) ==
	      "graph-file disk v3 running=v2\n"
	      "graph-file error 9:34 E2001: unexpected token: \"}\"\n"
	      "graph-file error 14:1 ear/include-unsupported: @include is not supported yet: write the whole graph in ear.conf\n");
}

TEST_CASE("A refusal, dropped edits, a pending restart and a failed write each have their line, in order",
          "[status_log]") {
	GraphFile everything = file("v9", "v8");
	everything.refusal = Refusal{ear::UnknownPort{{"rec", "FX"}}, TextPlace{14, 1}};
	everything.pendingRestart = {FormatKey::graphFrequency, FormatKey::blockSize};
	everything.writeFailed = "ear.conf: No space left on device";
	CHECK(printed({everything, FileChangeCause::disk, 2}) ==
	      "graph-file disk v9 running=v8\n"
	      "graph-file refused unknown-port at 14:1\n"
	      "graph-file dropped 2 edits\n"
	      "graph-file pending-restart graphFrequency blockSize\n"
	      "graph-file write-failed ear.conf: No space left on device\n");

	GraphFile unplaced = file("v4", std::nullopt);
	unplaced.refusal = Refusal{ear::Stopping{}, std::nullopt};
	CHECK(printed({unplaced, FileChangeCause::edit, 0}) == "graph-file edit v4 running=none\ngraph-file refused stopping\n");
}

TEST_CASE("A write prints its version", "[status_log]") {
	std::ostringstream out;
	StatusLog log(out);
	log.graphFileWritten("v7");
	CHECK(out.str() == "graph-file written v7\n");
}
