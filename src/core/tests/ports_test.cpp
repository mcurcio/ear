// A node type's ports are declared in banks, and one function makes a public port's whole name.
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/node_type.hpp"

using namespace ear;
using Names = std::vector<PortName>;

// Only a bank declares a public port: a list cannot be filled by hand.
static_assert(!std::is_aggregate_v<Ports>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<Ports&>().inputs())>>);

TEST_CASE("A port's whole name is its bank, a dot and its member", "[ports]") {
	CHECK(portName("from", "left") == "from.left");
	CHECK(portName("play", "1") == "play.1");
	CHECK(portName("a_b-C9", "d_e-F0") == "a_b-C9.d_e-F0");
}

TEST_CASE("A part that would mis-shape a port's name is a type bug", "[ports]") {
	const auto part = GENERATE(as<std::string>{}, "", "a.b", "a:b", "a b", "a\"b", "caf\xC3\xA9", std::string("a\0b", 3));
	CHECK_THROWS_AS(portName(part, "left"), std::logic_error);
	CHECK_THROWS_AS(portName("from", part), std::logic_error);
	CHECK_THROWS_AS((Ports{{{Direction::input, part, {"left"}}}}), std::logic_error);
	CHECK_THROWS_AS((Ports{{{Direction::output, "from", {part}}}}), std::logic_error);
}

TEST_CASE("Banks fill each direction in declaration order, members in order", "[ports]") {
	const Ports ports{{{Direction::output, "from", {"left", "right"}},
	                   {Direction::input, "to", {"left"}},
	                   {Direction::output, "stream", numbered(2)},
	                   {Direction::input, "play", {}}}};
	CHECK(ports.inputs() == Names{"to.left"});
	CHECK(ports.outputs() == Names{"from.left", "from.right", "stream.1", "stream.2"});
}

TEST_CASE("Inside ports keep the type's own names, and publicOnly leaves them out", "[ports]") {
	const Ports ports{{{Direction::input, "play", numbered(1)}, {Direction::output, "capture", numbered(1)}},
	                  numbered(1, "from_card_"),
	                  numbered(2, "to_card_")};
	CHECK(ports.insideInputs() == Names{"from_card_1"});
	CHECK(ports.insideOutputs() == Names{"to_card_1", "to_card_2"});

	const Ports only = ports.publicOnly();
	CHECK(only.inputs() == Names{"play.1"});
	CHECK(only.outputs() == Names{"capture.1"});
	CHECK(only.insideInputs().empty());
	CHECK(only.insideOutputs().empty());
	CHECK_FALSE(only == ports);
	CHECK(only == only.publicOnly());
}

TEST_CASE("numbered counts from 1, and none is none", "[ports]") {
	CHECK(numbered(3) == std::vector<std::string>{"1", "2", "3"});
	CHECK(numbered(2, "x_") == std::vector<std::string>{"x_1", "x_2"});
	CHECK(numbered(0).empty());
}
