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
	const auto part =
		GENERATE(as<std::string>{}, "", "a.b", "a:b", "a b", "a\"b", "caf\xC3\xA9", std::string("a\0b", 3));
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

TEST_CASE("Inside banks are named as routable ones are, and publicOnly leaves them out", "[ports]") {
	const Ports ports{{{Direction::input, "play", numbered(1)},
	                   {Direction::output, "to_card", numbered(2), PortBank::Reach::inside},
	                   {Direction::output, "capture", numbered(1)},
	                   {Direction::input, "from_card", numbered(1), PortBank::Reach::inside}}};
	CHECK(ports.inputs() == Names{"play.1"});
	CHECK(ports.outputs() == Names{"capture.1"});
	CHECK(ports.insideInputs() == Names{"from_card.1"});
	CHECK(ports.insideOutputs() == Names{"to_card.1", "to_card.2"});

	const Ports only = ports.publicOnly();
	CHECK(only.inputs() == Names{"play.1"});
	CHECK(only.outputs() == Names{"capture.1"});
	CHECK(only.insideInputs().empty());
	CHECK(only.insideOutputs().empty());
	CHECK(only.banks().size() == 2);
	CHECK_FALSE(only == ports);
	CHECK(only == only.publicOnly());
}

TEST_CASE("A bank says where its ports sit in what process() gets: routable ports, then inside ones", "[ports]") {
	const Ports ports{{{Direction::output, "to_card", numbered(2), PortBank::Reach::inside},
	                   {Direction::input, "play", numbered(2)},
	                   {Direction::output, "capture", numbered(3)},
	                   {Direction::input, "from_card", numbered(3), PortBank::Reach::inside},
	                   {Direction::input, "extra", {"a"}}}};
	CHECK(ports.bank("play").first == 0);
	CHECK(ports.bank("extra").first == 2);
	CHECK(ports.bank("from_card").first == 3); // after play and extra
	CHECK(ports.bank("capture").first == 0);
	CHECK(ports.bank("to_card").first == 3); // after capture
	CHECK(ports.bank("to_card").ports == Names{"to_card.1", "to_card.2"});
	CHECK(ports.bank("to_card").reach == PortBank::Reach::inside);
	CHECK(ports.bank("play").direction == Direction::input);
	CHECK_THROWS_AS(ports.bank("nope"), std::logic_error);
}

TEST_CASE("Two banks of one name are a type bug", "[ports]") {
	CHECK_THROWS_AS((Ports{{{Direction::input, "play", {"1"}}, {Direction::output, "play", {"2"}}}}), std::logic_error);
	CHECK_THROWS_AS(
		(Ports{{{Direction::input, "play", {"1"}}, {Direction::input, "play", {"2"}, PortBank::Reach::inside}}}),
		std::logic_error);
}

TEST_CASE("numbered counts from 1, and none is none", "[ports]") {
	CHECK(numbered(3) == std::vector<std::string>{"1", "2", "3"});
	CHECK(numbered(0).empty());
}
