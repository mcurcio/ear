// The plain types of ear.conf's state (docs/design/knobly-config.md, "Wire additions"): the severity
// names the wire carries, and value equality of places and diagnostics.
#include "catch2/catch.hpp"
#include "ear/control/graph_file_state.hpp"

using namespace ear::control;

TEST_CASE("Severities are named as the wire writes them", "[graph_file_state]") {
	CHECK(severityName(Severity::error) == "error");
	CHECK(severityName(Severity::warning) == "warning");
}

TEST_CASE("Places and diagnostics compare by value", "[graph_file_state]") {
	const Diagnostic typo{Severity::error, "E2001", "unexpected token", TextPlace{3, 7}};
	CHECK(typo == Diagnostic{Severity::error, "E2001", "unexpected token", TextPlace{3, 7}});
	CHECK_FALSE(typo == Diagnostic{Severity::error, "E2001", "unexpected token", TextPlace{3, 8}});
	CHECK_FALSE(typo == Diagnostic{Severity::warning, "E2001", "unexpected token", TextPlace{3, 7}});
	CHECK_FALSE(typo == Diagnostic{Severity::error, "ear/bad-directive", "unexpected token", TextPlace{3, 7}});
}
