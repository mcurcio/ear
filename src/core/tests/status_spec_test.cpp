// A type's status fields, events and log codes in its spec (docs/writing-a-node-type.md#status-and-events): every
// spelling of a TypeSpec deduces with them, their declarations are what the makers say, and the type's schema and its
// configs carry them. What must not compile is compile_fail/status_spec_bad.cpp's.
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"

namespace status_spec_test {

using namespace ear;

constexpr SettingsTable<> table{"probe"};
constexpr auto out = bank::output<"out">;
struct WorkerState {};
constexpr auto worker = loop::strand<"worker", WorkerState>;
constexpr auto file = store::file<"key">({.maxBytes = 64});

constexpr std::string_view connections[] = {"dialling", "listening", "connected"};
constexpr auto connection = status::choice<"connection">(writer::loop(worker), connections)
                                .display(Display::box)
                                .help("how it reaches a server");
constexpr auto paired = status::flag<"paired">(writer::loop(worker));
constexpr auto streamRate = status::number<"streamRate">(writer::loop(worker), 0, 384000, "Hz").initially(48000);
constexpr auto peakDb = status::decimal<"peakDb">(writer::rt, -90.0, 24.0, "dB", 1).flapsAbove(4, 2000);
constexpr auto pairingCode = status::text<"pairingCode">(writer::loop(worker), 16).display(Display::callout);
constexpr auto mode = status::choice<"mode">(writer::loop(worker), connections).initially("listening");
constexpr auto quiet = status::flag<"quiet">(writer::companion).initially(status::unknown);

constexpr std::string_view reasons[] = {"attempt-timeout", "pairing-code-mismatch"};
constexpr auto pairingFailed =
	event::of<"pairing-failed">(EventSeverity::warn, "pairing failed: {reason}", arg::choice<"reason">(reasons));
constexpr auto clipped = event::of<"clipped">(EventSeverity::error, "channel {channel} clipped at {peak} dB",
                                              arg::number<"channel">(1, 64), arg::decimal<"peak">(1));
constexpr auto started = event::of<"started">(EventSeverity::info, "started");
constexpr auto overrun = log::code<"overrun">(LogLevel::warn, "overran by {us} us", arg::number<"us">(0, 1'000'000));

// Every spelling: each new member alone after each built one, and all together, deduce the rest.
constexpr TypeSpec statusAfterBanks{.settings = table, .banks = Banks<out>{}, .status = StatusTable<peakDb>{}};
constexpr TypeSpec eventsAfterBanks{.settings = table, .banks = Banks<out>{}, .events = EventTable<started>{}};
constexpr TypeSpec logCodesAfterBanks{.settings = table, .banks = Banks<out>{}, .logCodes = LogCodes<overrun>{}};
constexpr TypeSpec statusAfterStores{
	.settings = table, .banks = Banks<out>{}, .stores = Stores<file>{}, .status = StatusTable<peakDb>{}};
constexpr TypeSpec eventsAfterLoops{
	.settings = table, .banks = Banks<out>{}, .loops = Loops<worker>{}, .events = EventTable<pairingFailed>{}};
constexpr TypeSpec logCodesAfterLifecycle{.settings = table,
                                          .banks = Banks<out>{},
                                          .lifecycle = {.reconfigureDeadline = std::chrono::seconds(5)},
                                          .logCodes = LogCodes<overrun>{}};
constexpr TypeSpec whole{.settings = table,
                         .banks = Banks<out>{},
                         .stores = Stores<file>{},
                         .loops = Loops<worker>{},
                         .lifecycle = {},
                         .status = StatusTable<connection, paired, streamRate, peakDb, pairingCode, mode, quiet>{},
                         .events = EventTable<pairingFailed, clipped, started>{},
                         .logCodes = LogCodes<overrun>{}};

struct Silence {
	void process(Io<whole> io, const GraphCycle&) noexcept {
		for (const auto member : io[out]) std::ranges::fill(member, 0.0F);
	}
};
struct Probe {
	static Members<whole> ports(const CheckedOf<whole>&) { return {out(members("mono"))}; }
	Configured configure(CreateOf<whole>& cx, const CheckedOf<whole>&) {
		return cx.applied(Silence{}, cx.state(worker));
	}
};

TEST_CASE("Every spelling of a spec deduces with status fields, events and log codes", "[status-spec]") {
	STATIC_REQUIRE(StatusOf<statusAfterBanks>::count == 1);
	STATIC_REQUIRE(EventsOf<statusAfterBanks>::count == 0);
	STATIC_REQUIRE(EventsOf<eventsAfterBanks>::count == 1);
	STATIC_REQUIRE(LogCodesOf<logCodesAfterBanks>::count == 1);
	STATIC_REQUIRE(StatusOf<statusAfterStores>::count == 1);
	STATIC_REQUIRE(EventsOf<eventsAfterLoops>::count == 1);
	STATIC_REQUIRE(LogCodesOf<logCodesAfterLifecycle>::count == 1);
	STATIC_REQUIRE(logCodesAfterLifecycle.lifecycle.reconfigureDeadline == std::chrono::seconds(5));
	STATIC_REQUIRE(StatusOf<whole>::count == 7);
	STATIC_REQUIRE(StatusOf<whole>::has<paired>);
	STATIC_REQUIRE(!StatusOf<whole>::has<connections>);
	STATIC_REQUIRE(StatusOf<whole>::indexOf<streamRate> == 2);
	STATIC_REQUIRE(EventsOf<whole>::indexOf<started> == 2);
}

TEST_CASE("A status field's declaration is what its maker and marks say, with its initial value", "[status-spec]") {
	const StatusDeclaration& c = connection.declaration();
	CHECK(c.name == "connection");
	CHECK(c.kind == ValueKind::choice);
	CHECK(c.writer.kind == writer::Writer::Kind::loop);
	CHECK(c.writer.loop == "worker");
	CHECK(c.display == Display::box);
	CHECK(c.help == "how it reaches a server");
	CHECK(c.values.size() == 3);
	CHECK(c.flapsAbove.transitions == 10);
	CHECK(c.flapsAbove.windowMs == 10'000);
	CHECK(std::holds_alternative<std::monostate>(c.initial())); // a choice starts unknown
	CHECK(paired.declaration().initial() == StatusValue(false));
	CHECK(streamRate.declaration().initial() == StatusValue(std::int64_t{48000}));
	CHECK(streamRate.declaration().unit == "Hz");
	CHECK(peakDb.declaration().initial() == StatusValue(0.0));
	CHECK(peakDb.declaration().decimals == 1);
	CHECK(peakDb.declaration().flapsAbove.transitions == 4);
	CHECK(peakDb.declaration().flapsAbove.windowMs == 2000);
	CHECK(pairingCode.declaration().initial() == StatusValue(std::string()));
	CHECK(pairingCode.declaration().maxBytes == 16);
	CHECK(mode.declaration().initial() == StatusValue(std::string("listening")));
	CHECK(std::holds_alternative<std::monostate>(quiet.declaration().initial()));
	STATIC_REQUIRE(connection("connected") == Choice{2});
}

TEST_CASE("An event's and a log code's declarations carry their arguments in order", "[status-spec]") {
	const EventDeclaration c = clipped.declaration();
	CHECK(c.code == "clipped");
	CHECK(c.severity == EventSeverity::error);
	CHECK(c.sentence == "channel {channel} clipped at {peak} dB");
	REQUIRE(c.args.size() == 2);
	CHECK(c.args[0].name == "channel");
	CHECK(c.args[0].kind == ValueKind::wholeNumber);
	CHECK(c.args[0].high == 64);
	CHECK(c.args[1].kind == ValueKind::decimal);
	CHECK(c.args[1].decimals == 1);
	CHECK(pairingFailed.declaration().args[0].values.size() == 2);
	CHECK(started.declaration().args.empty());
	CHECK(overrun.declaration().level == LogLevel::warn);
	CHECK(overrun.declaration().args.size() == 1);
}

TEST_CASE("A type's schema and each of its configs carry its status fields and events in declaration order",
          "[status-spec]") {
	const auto type = makeNodeType<whole, Probe>();
	const TypeSchema schema = type->schema();
	REQUIRE(schema.status.size() == 7);
	CHECK(schema.status[0]->name == "connection");
	CHECK(schema.status[6]->name == "quiet");
	REQUIRE(schema.events.size() == 3);
	CHECK(schema.events[0].code == "pairing-failed");
	CHECK(schema.events[2].code == "started");
	const auto config = type->parse("node", NodeSettings::object());
	REQUIRE(config);
	CHECK((*config)->statusDeclared().size() == 7);
	CHECK((*config)->eventsDeclared().size() == 3);
}

} // namespace status_spec_test
