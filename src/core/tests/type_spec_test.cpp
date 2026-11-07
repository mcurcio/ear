// The type SPI (docs/design/node-framework.md sections 7.1, 7.2, 7.6 and 9.2, slice 2): a spec, a behaviour, makeNodeType,
// Io by bank and the harness that runs a type without PipeWire. What must not compile is
// compile_fail/type_spec_bad.cpp's.
#include <concepts>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/type_spec.hpp"
#include "ear/testing/type_harness.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace type_spec_test {

using namespace ear;

std::optional<std::string> anyText(std::string_view text) { return std::string(text); }

constexpr auto channelsRow = setting::defaulted("channels", setting::number(1, 8), 2);
constexpr auto modeRow = setting::optional("mode", setting::text<anyText>("anything"));
constexpr SettingsTable<channelsRow, modeRow> table{"probe"};
constexpr auto in = bank::input<"in">;
constexpr auto out = bank::output<"out">;
constexpr auto toDevice = bank::insideOutput<"to_device">;
constexpr auto fromDevice = bank::insideInput<"from_device">;
constexpr TypeSpec spec{.settings = table, .banks = Banks<in, out, toDevice, fromDevice>{}};
using Checked = CheckedOf<spec>;

/// out = 2 * in, to_device = in; counts its prepare and release.
struct Doubler {
	int* prepared;
	void prepare(const AudioFormat&) { ++*prepared; }
	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (std::size_t member = 0; member < io[in].size(); ++member) {
			for (std::uint32_t frame = 0; frame < io.frames(); ++frame) {
				io[out][member][frame] = 2.0F * io[in][member][frame];
				io[toDevice][member][frame] = io[in][member][frame];
			}
		}
	}
};

/// A run object with no threads: it closes at once.
class ProbeRun final : public RunObject {};

int prepares = 0;

struct Probe {
	static Members<spec> ports(const Checked& settings) {
		const std::uint32_t n = settings.get<channelsRow>();
		return {in(members(n)), out(members(n)), toDevice(members(n)), fromDevice(members(n))};
	}
	static std::expected<void, std::string> check(const Checked& settings) {
		if (settings.get<modeRow>() == "bad") return std::unexpected(std::string("mode: not bad"));
		return {};
	}
	static StartResult start(StartContext& context, const Checked& settings) {
		const auto mode = settings.get<modeRow>();
		if (mode == "card") {
			context.acquire(SoundCardRequest{
				.card = *CardId::parse("Probe"), .direction = CardDirection::playback, .bank = "to_device"});
			return {passThrough({bankPair(in, toDevice), bankPair(fromDevice, out)}),
			        [](Ready& ready) -> std::expected<std::unique_ptr<RunObject>, FailureReason> {
						ready.events().say("built");
						return std::make_unique<ProbeRun>();
					}};
		}
		if (mode == "fail") return std::unexpected(FailureReason{FailureReason::Code::startFailed, "asked to"});
		return Doubler{&prepares};
	}
};

using Harness = testing::TypeHarness<spec, Probe>;

// Another type's spec. Its bank "out" has this spec's name, direction and reach, so it is the same handle: Io takes it
// and resolves it by name against the node's own ports (sound, and accepted). A bank this spec lacks does not compile
// (compile_fail/type_spec_bad.cpp, io-bank-the-spec-lacks).
constexpr auto otherOut = bank::output<"out">;
constexpr TypeSpec otherSpec{.settings = SettingsTable<>{"other"}, .banks = Banks<otherOut>{}};
static_assert(std::same_as<decltype(otherOut), decltype(out)>);
static_assert(requires(Io<spec> io) { io[otherOut]; });

/// A code written against the other spec.
struct OtherCode {
	void process(Io<otherSpec>, const GraphCycle&) noexcept {}
};
/// A behaviour of this spec whose start returns the other spec's code.
struct Mixed : Probe {
	static StartResult start(StartContext&, const Checked&) { return OtherCode{}; }
};

} // namespace type_spec_test
using namespace type_spec_test;

TEST_CASE("A spec's banks become the node's ports, in the spec's order", "[type-spec]") {
	Harness harness;
	CHECK(harness.type().name() == "probe");
	REQUIRE(harness->parse({{"channels", 2}}));
	CHECK(harness->ports().inputs() == std::vector<PortName>{"in.1", "in.2"});
	CHECK(harness->ports().outputs() == std::vector<PortName>{"out.1", "out.2"});
	CHECK(harness->ports().insideOutputs() == std::vector<PortName>{"to_device.1", "to_device.2"});
	CHECK(harness->ports().insideInputs() == std::vector<PortName>{"from_device.1", "from_device.2"});
	CHECK(harness->ports().bank("from_device").first == 2);
}

TEST_CASE("check runs after the table's walk, and its refusal is the parse's", "[type-spec]") {
	Harness harness;
	CHECK(harness->parse({{"channels", 9}}).error() == "channels: must be a whole number, 1-8");
	CHECK(harness->parse({{"mode", "bad"}}).error() == "mode: not bad");
	const auto parsed = harness.type().parse("p", {{"mode", "bad"}});
	REQUIRE_FALSE(parsed);
	CHECK(parsed.error().node == "p");
	CHECK(parsed.error().detail == "mode: not bad");
}

TEST_CASE("A code reads and writes by bank through Io, prepared once and released at close", "[type-spec]") {
	prepares = 0;
	Harness harness({.rate = 48000, .maxBlock = 64});
	REQUIRE(harness->parse({{"channels", 2}}));
	REQUIRE(harness->start());
	CHECK(harness->state() == testing::NodeHarness::State::running);
	CHECK(harness->runObject() == nullptr);
	CHECK(prepares == 1);
	std::ranges::fill(harness.input(in, 0), 0.25F);
	std::ranges::fill(harness.input(in, 1), -0.5F);
	harness->process({.startNs = 0, .position = 0}, 32);
	CHECK(harness->unwritten() == 0);
	CHECK(harness.output(out, 0)[31] == 0.5F);
	CHECK(harness.output(out, 1)[0] == -1.0F);
	CHECK(harness.output(toDevice, 1)[5] == -0.5F);
	harness->close();
	CHECK(harness->state() == testing::NodeHarness::State::closed);
}

TEST_CASE("The harness readies and loses outside resources, then builds the run object", "[type-spec]") {
	Harness harness;
	REQUIRE(harness->parse({{"mode", "card"}, {"channels", 1}}));
	REQUIRE(harness->start());
	CHECK(harness->state() == testing::NodeHarness::State::starting);
	REQUIRE(harness->outsideCount() == 1);
	CHECK(harness->outsideName(0) == "node.playback");
	REQUIRE(harness->requests().size() == 1);
	CHECK(std::get<SoundCardRequest>(harness->requests()[0]).bank == "to_device");
	harness->ready(0);
	CHECK(harness->state() == testing::NodeHarness::State::running);
	CHECK(harness->runObject() != nullptr);
	CHECK(harness->notes() == std::vector<std::string>{"built"});
	std::ranges::fill(harness.input(in, 0), 0.75F);
	std::ranges::fill(harness->input("from_device.1"), 0.125F);
	harness->process({.startNs = 0, .position = 0}, 16);
	CHECK(harness->unwritten() == 0);
	CHECK(harness.output(toDevice, 0)[15] == 0.75F);
	CHECK(harness.output(out, 0)[0] == 0.125F);
	harness->lose(0, "card gone");
	CHECK(harness->state() == testing::NodeHarness::State::failed);
	CHECK(harness->failure()->code == FailureReason::Code::engineRefused);
	CHECK(harness->failure()->detail == "card gone");
	harness->close();
}

TEST_CASE("A start that fails is the harness's failure", "[type-spec]") {
	Harness harness;
	REQUIRE(harness->parse({{"mode", "fail"}}));
	const auto started = harness->start();
	REQUIRE_FALSE(started);
	CHECK(started.error().detail == "asked to");
	CHECK(harness->state() == testing::NodeHarness::State::failed);
}

TEST_CASE("A layout that disagrees with the spec is a logic error at prepare", "[type-spec]") {
	// Missing a bank, a bank of the wrong direction, and a code run outside makeNodeType.
	const Ports missing{{{Direction::input, "in", {"1"}}, {Direction::output, "out", {"1"}}}};
	CHECK_THROWS_AS(Io<spec>::resolve(missing), std::logic_error);
	const Ports turned{{{Direction::input, "in", {"1"}},
	                    {Direction::output, "out", {"1"}},
	                    {Direction::input, "to_device", {"1"}, PortBank::Reach::inside},
	                    {Direction::input, "from_device", {"1"}, PortBank::Reach::inside}}};
	CHECK_THROWS_AS(Io<spec>::resolve(turned), std::logic_error);
	int unused = 0;
	const auto erased = eraseSpecCode(Doubler{&unused});
	CHECK_THROWS_WITH(erased->prepare({48000, 64}),
	                  "a code written against a spec runs only in a type made by makeNodeType");
}

TEST_CASE("Members are numbered or named, each once", "[type-spec]") {
	CHECK(members(3).names == std::vector<std::string>{"1", "2", "3"});
	CHECK(members("left", "right").names == std::vector<std::string>{"left", "right"});
	const Members<spec> twice{in(members("a", "a")), out(members(1)), toDevice(members(1)), fromDevice(members(1))};
	CHECK_THROWS_AS(twice.ports(), std::logic_error);
}

TEST_CASE("A start that returns a code written against another type's spec is a logic error", "[type-spec]") {
	testing::TypeHarness<spec, Mixed> harness;
	REQUIRE(harness->parse({{"channels", 1}}));
	CHECK_THROWS_WITH(harness->start(), "a type's start returned a code written against another type's spec");
}
