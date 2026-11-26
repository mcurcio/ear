// The node types' schemas on the wire (docs/control-port.md#type-schemas): each row kind and mark as types.get says it,
// the hash, and types.get's params and answers. Every built-in type's schema is test/protocol/types.json's, which the
// system tests hold the real ear to.
#include "ear/control/type_schemas.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <expected>
#include <string>
#include <string_view>

#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace type_schemas_test {

using nlohmann::json;
namespace setting = ear::setting;

inline constexpr std::array<std::string_view, 2> shapes{"round", "square"};
std::optional<std::string> anyText(std::string_view text) { return std::string(text); }

inline constexpr auto deviceRow =
	setting::required("device", setting::text<anyText>("a device", {.pattern = "^[a-z]+$", .maxLength = 8}))
		.restarts("the device opens once");
inline constexpr auto shapeRow =
	setting::optional("shape", setting::text<anyText>("round or square", {.values = shapes})).inPlace();
inline constexpr auto countRow = setting::defaulted("count", setting::number(1, 8, "channels"), 2)
                                     .shapesPorts()
                                     .restarts("the count is read at start");
inline constexpr auto rateRow =
	setting::optional("rate", setting::rate(ear::SampleRate::fixed<8'000>(), ear::SampleRate::fixed<96'000>()))
		.defaultFrom(setting::DefaultFrom::graphFrequency)
		.inPlace();
inline constexpr auto loudRow =
	setting::optional("loud", setting::boolean()).help("absent: quiet").restarts("the volume is read at start");
inline constexpr auto portRow =
	setting::optional("port", setting::number(1, 9)).onlyWithout(shapeRow).restarts("the port is bound at start");
inline constexpr auto in = ear::bank::input<"in">;
inline constexpr auto out = ear::bank::output<"out">;
inline constexpr auto inside = ear::bank::insideInput<"inside">;
inline constexpr ear::TypeSpec spec{
	.settings = ear::SettingsTable<deviceRow, shapeRow, countRow, rateRow, loudRow, portRow>{"every"},
	.banks = ear::Banks<in, inside, out>{},
	.lifecycle = {.reconfigureDeadline = std::chrono::seconds(5)}};
struct Every {
	static ear::Members<spec> ports(const ear::CheckedOf<spec>& s) {
		return {in(ear::members(s.get<countRow>())), inside(ear::members(1)), out(ear::members(1))};
	}
	ear::Configured configure(ear::CreateOf<spec>&, const ear::CheckedOf<spec>&) {
		return ear::Configured::failed("not started in these tests");
	}
	ear::Changed configure(ear::ChangeOf<spec>&, const ear::CheckedOf<spec>&, const ear::CheckedOf<spec>&) {
		return ear::Changed::restart("not changed in these tests");
	}
};

} // namespace type_schemas_test

namespace reporting {
using namespace ear;
struct State;
inline constexpr auto worker = loop::strand<"worker", State>;
inline constexpr std::string_view states[] = {"dialling", "connected"};
inline constexpr auto connection =
	status::choice<"connection">(writer::loop(worker), states).display(Display::box).help("how it reaches a server");
inline constexpr auto paired = status::flag<"paired">(writer::loop(worker)).flapsAbove(4, 2000);
inline constexpr auto rate = status::number<"rate">(writer::loop(worker), 0, 192000, "Hz").initially(48000);
inline constexpr auto peak = status::decimal<"peak">(writer::rt, -90.0, 6.0, "dB", 1);
inline constexpr auto code = status::text<"code">(writer::loop(worker), 16).display(Display::callout);
inline constexpr auto failed =
	event::of<"pairing-failed">(EventSeverity::warn, "pairing failed: {reason} after {tries} tries",
                                arg::choice<"reason">(states), arg::number<"tries">(1, 9));
inline constexpr auto said =
	event::of<"said">(EventSeverity::info, "said {what} at {level}", arg::text<"what">(32), arg::decimal<"level">());
inline constexpr auto out = bank::output<"out">;
inline constexpr auto serverRow = setting::required("server", setting::number(1, 99)).help("its number");
inline constexpr auto shoutRow = setting::optional("shout", setting::boolean());
inline constexpr SettingsTable<serverRow, shoutRow> forgetArgs{"forget"};
inline constexpr auto resetPeak = action::trigger<"reset-peak">("clear the peak");
inline constexpr auto forget = action::call<"forget", forgetArgs, worker>("forget a server");
inline constexpr TypeSpec spec{.settings = SettingsTable<>{"reporting"},
                               .banks = Banks<out>{},
                               .loops = Loops<worker>{},
                               .status = StatusTable<connection, paired, rate, peak, code>{},
                               .events = EventTable<failed, said>{},
                               .actions = ActionTable<resetPeak, forget>{}};
struct State {
	std::expected<void, std::string> act(Invoked<forget>, const CheckedOf<forgetArgs>&) { return {}; }
};
struct Silence {
	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (const auto member : io[out]) std::ranges::fill(member, 0.0F);
	}
};
struct Reporting {
	static Members<spec> ports(const CheckedOf<spec>&) { return {out(members("mono"))}; }
	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) { return cx.applied(Silence{}, cx.state(worker)); }
};
} // namespace reporting

using namespace type_schemas_test;

TEST_CASE("Each row kind and mark is on the wire as the control port takes the key", "[types][protocol]") {
	const auto type = ear::makeNodeType<spec, Every>();
	const json schema = ear::control::encodeTypeSchema(type->schema());
	CHECK(schema == json::parse(R"({"name": "every", "inputs": ["in"], "outputs": ["out"], "reconfigureMs": 5000,
	  "settings": [
	    {"key": "device", "kind": "text", "required": true, "change": "restart", "restartReason": "the device opens once", "mustBe": "a device",
	     "pattern": "^[a-z]+$", "maxLength": 8},
	    {"key": "shape", "kind": "text", "required": false, "change": "inPlace", "mustBe": "round or square",
	     "values": ["round", "square"]},
	    {"key": "count", "kind": "wholeNumber", "required": false, "change": "restart", "restartReason": "the count is read at start", "min": 1, "max": 8,
	     "unit": "channels", "default": 2, "shapes": true},
	    {"key": "rate", "kind": "wholeNumber", "required": false, "change": "inPlace", "min": 8000, "max": 96000,
	     "unit": "Hz", "defaultFrom": "graphFrequency"},
	    {"key": "loud", "kind": "flag", "required": false, "change": "restart", "restartReason": "the volume is read at start",
	     "help": "absent: quiet"},
	    {"key": "port", "kind": "wholeNumber", "required": false, "change": "restart", "restartReason": "the port is bound at start", "min": 1, "max": 9,
	     "onlyWithout": "shape"}],
	  "status": [], "events": [], "actions": []})"));
}

TEST_CASE("A type's actions are on the wire in declaration order, a call's arguments as setting rows",
          "[types][protocol][actions]") {
	const auto type = ear::makeNodeType<reporting::spec, reporting::Reporting>();
	const json schema = ear::control::encodeTypeSchema(type->schema());
	CHECK(schema["actions"] == json::parse(R"([
	  {"name": "reset-peak", "by": "type", "target": "code", "help": "clear the peak", "args": []},
	  {"name": "forget", "by": "type", "target": "loop", "help": "forget a server", "args": [
	    {"key": "server", "kind": "wholeNumber", "required": true, "min": 1, "max": 99, "help": "its number"},
	    {"key": "shout", "kind": "flag", "required": false}]}])"));
}

TEST_CASE("A type's status fields and events are on the wire in declaration order, each with what applies",
          "[types][protocol][status]") {
	const auto type = ear::makeNodeType<reporting::spec, reporting::Reporting>();
	const json schema = ear::control::encodeTypeSchema(type->schema());
	CHECK(schema["status"] == json::parse(R"([
	  {"name": "connection", "kind": "choice", "initial": null, "display": "box",
	   "flapsAbove": {"transitions": 10, "windowMs": 10000}, "values": ["dialling", "connected"],
	   "help": "how it reaches a server"},
	  {"name": "paired", "kind": "flag", "initial": false, "display": "panel",
	   "flapsAbove": {"transitions": 4, "windowMs": 2000}},
	  {"name": "rate", "kind": "wholeNumber", "initial": 48000, "display": "panel",
	   "flapsAbove": {"transitions": 10, "windowMs": 10000}, "min": 0, "max": 192000, "unit": "Hz"},
	  {"name": "peak", "kind": "decimal", "initial": 0.0, "display": "panel",
	   "flapsAbove": {"transitions": 10, "windowMs": 10000}, "min": -90.0, "max": 6.0, "decimals": 1, "unit": "dB"},
	  {"name": "code", "kind": "text", "initial": "", "display": "callout",
	   "flapsAbove": {"transitions": 10, "windowMs": 10000}, "maxBytes": 16}])"));
	CHECK(schema["events"] == json::parse(R"([
	  {"code": "pairing-failed", "severity": "warn", "sentence": "pairing failed: {reason} after {tries} tries",
	   "args": [{"name": "reason", "kind": "choice", "values": ["dialling", "connected"]},
	            {"name": "tries", "kind": "wholeNumber", "min": 1, "max": 9}]},
	  {"code": "said", "severity": "info", "sentence": "said {what} at {level}",
	   "args": [{"name": "what", "kind": "text", "maxBytes": 32}, {"name": "level", "kind": "decimal", "decimals": 2}]}])"));
}

TEST_CASE("The hash is FNV-1a 64 of the schemas' encoding, written with its name", "[types][protocol]") {
	CHECK(ear::control::fnv1a64("") == "fnv1a64:cbf29ce484222325");
	CHECK(ear::control::fnv1a64("a") == "fnv1a64:af63dc4c8601ec8c");
	const auto type = ear::makeNodeType<spec, Every>();
	const ear::TypeSchema schemas[] = {type->schema()};
	const ear::control::TypeSchemas encoded = ear::control::encodeTypeSchemas(schemas);
	REQUIRE(encoded.typeSchemas.size() == 1);
	CHECK(encoded.hash == ear::control::fnv1a64(encoded.typeSchemas.dump()));
	CHECK(ear::control::encodeTypeSchemas(schemas).hash == encoded.hash); // the same set, the same hash
}

TEST_CASE("types.get takes no params or ifNoneMatch, and answers notModified for the hash", "[types][protocol]") {
	using ear::control::decodeGetTypes;
	CHECK(decodeGetTypes(nullptr).value() == std::nullopt);
	CHECK(decodeGetTypes(json::object()).value() == std::nullopt);
	CHECK(decodeGetTypes(json{{"ifNoneMatch", "fnv1a64:1"}}).value() == "fnv1a64:1");
	const auto extra = decodeGetTypes(json{{"ifNoneMatch", "x"}, {"colour", 1}});
	REQUIRE_FALSE(extra.has_value());
	CHECK(extra.error().code == ear::control::invalidParams);
	CHECK(extra.error().data == json{{"pointer", "/colour"}});
	const auto mistyped = decodeGetTypes(json{{"ifNoneMatch", 7}});
	REQUIRE_FALSE(mistyped.has_value());
	CHECK(mistyped.error().message == "/ifNoneMatch: must be a string");
	CHECK(mistyped.error().data == json{{"pointer", "/ifNoneMatch"}});
	CHECK_FALSE(decodeGetTypes(json::array({1})).has_value());

	const ear::control::TypeSchemas types{"fnv1a64:0123456789abcdef", json::array({{{"name", "white"}}})};
	CHECK(ear::control::typesResult(types, std::nullopt) ==
	      json{{"protocol", 1}, {"hash", types.hash}, {"typeSchemas", types.typeSchemas}});
	CHECK(ear::control::typesResult(types, "fnv1a64:stale") ==
	      json{{"protocol", 1}, {"hash", types.hash}, {"typeSchemas", types.typeSchemas}});
	CHECK(ear::control::typesResult(types, types.hash) ==
	      json{{"protocol", 1}, {"hash", types.hash}, {"notModified", true}});
}
