// The node types' schemas on the wire (docs/control-port.md#type-schemas): each row kind and mark as types.get says it,
// the hash, and types.get's params and answers. Every built-in type's schema is test/protocol/types.json's, which the
// system tests hold the real ear to.
#include "ear/control/type_schemas.hpp"

#include <array>
#include <chrono>
#include <optional>
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
	setting::required("device", setting::text<anyText>("a device", {.pattern = "^[a-z]+$", .maxLength = 8}));
inline constexpr auto shapeRow =
	setting::optional("shape", setting::text<anyText>("round or square", {.values = shapes})).inPlace();
inline constexpr auto countRow = setting::defaulted("count", setting::number(1, 8, "channels"), 2).shapesPorts();
inline constexpr auto rateRow =
	setting::optional("rate", setting::rate(ear::SampleRate::fixed<8'000>(), ear::SampleRate::fixed<96'000>()))
		.defaultFrom(setting::DefaultFrom::graphFrequency)
		.inPlace();
inline constexpr auto loudRow = setting::optional("loud", setting::boolean()).help("absent: quiet");
inline constexpr auto portRow = setting::optional("port", setting::number(1, 9)).onlyWithout(shapeRow);
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
using namespace type_schemas_test;

TEST_CASE("Each row kind and mark is on the wire as the control port takes the key", "[types][protocol]") {
	const auto type = ear::makeNodeType<spec, Every>();
	const json schema = ear::control::encodeTypeSchema(type->schema());
	CHECK(schema == json::parse(R"({"name": "every", "inputs": ["in"], "outputs": ["out"], "reconfigureMs": 5000,
	  "settings": [
	    {"key": "device", "kind": "text", "required": true, "change": "restart", "mustBe": "a device",
	     "pattern": "^[a-z]+$", "maxLength": 8},
	    {"key": "shape", "kind": "text", "required": false, "change": "inPlace", "mustBe": "round or square",
	     "values": ["round", "square"]},
	    {"key": "count", "kind": "wholeNumber", "required": false, "change": "restart", "min": 1, "max": 8,
	     "unit": "channels", "default": 2, "shapes": true},
	    {"key": "rate", "kind": "wholeNumber", "required": false, "change": "inPlace", "min": 8000, "max": 96000,
	     "unit": "Hz", "defaultFrom": "graphFrequency"},
	    {"key": "loud", "kind": "flag", "required": false, "change": "restart", "help": "absent: quiet"},
	    {"key": "port", "kind": "wholeNumber", "required": false, "change": "restart", "min": 1, "max": 9,
	     "onlyWithout": "shape"}]})"));
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
