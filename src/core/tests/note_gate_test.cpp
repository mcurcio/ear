// The note rate on its own clock (docs/writing-a-node-type.md#the-node-handle): what each configuration's handle judges
// a note by. Through a real NodeHost, per node, is node_handle_test.cpp's.
#include <chrono>
#include <cstdint>
#include <optional>

#include "catch2/catch.hpp"
#include "note_gate.hpp"

using ear::detail::NoteGate;
using namespace std::chrono_literals;

TEST_CASE("A burst of notes passes, then one a second, and what was held back is counted once one can pass",
          "[note_gate]") {
	std::chrono::steady_clock::time_point now{};
	NoteGate gate([&] { return now; });
	int passed = 0;
	for (int i = 0; i < 1000; ++i) {
		const std::optional<std::uint64_t> held = gate.pass();
		if (!held) continue;
		CHECK(*held == 0);
		++passed;
	}
	CHECK(passed == 32);
	CHECK_FALSE(gate.pass()); // too soon: counted too

	now += 1s;
	CHECK(gate.pass() == std::optional<std::uint64_t>(969));
	CHECK_FALSE(gate.pass());
	now += 1s;
	CHECK(gate.pass() == std::optional<std::uint64_t>(1)); // the count began again

	now += 1h; // a long quiet: a burst again, no more
	passed = 0;
	for (int i = 0; i < 100; ++i) passed += gate.pass() ? 1 : 0;
	CHECK(passed == 32);
}

TEST_CASE("Half a second refills half a note: the rate is one a second, not one per call after a pause",
          "[note_gate]") {
	std::chrono::steady_clock::time_point now{};
	NoteGate gate([&] { return now; });
	for (int i = 0; i < 32; ++i) REQUIRE(gate.pass());
	now += 500ms;
	CHECK_FALSE(gate.pass());
	now += 500ms;
	CHECK(gate.pass() == std::optional<std::uint64_t>(1));
}

TEST_CASE("The held-back count is said as the framework's own note text", "[note_gate]") {
	CHECK(ear::detail::notesDropped(969) == "notes-dropped 969");
}
