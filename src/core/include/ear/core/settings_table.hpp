// One node type's settings, each key declared once, as typed rows in one table. The table checks the settings object in
// one fixed order and makes every refusal's text from the row; NodeType's name(), settingKinds() and parse() all answer
// from it and cannot be overridden, so the graph file's reader cannot disagree with parse. A type's class gets typed
// values (CheckedOf<spec>, lifecycle.hpp), never the raw object, and its `check` holds any rule about them.
// Pure: nlohmann/json only.
#pragma once
#include <algorithm>
#include <any>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include "ear/core/graph_spec.hpp"
#include "ear/core/node_type.hpp"
#include "ear/core/sample_rate.hpp"
namespace ear {
namespace setting {
/// Where an optional row's value comes from when ear.conf does not give it (Row::defaultFrom): the graph's frequency
/// (an rtp node's sender rate), or the node's own name (a speaker's name). Documentation for controls and the type
/// schema (docs/writing-a-node-type.md#the-type-schema); the type's code still reads the row as absent and applies it.
enum class DefaultFrom { graphFrequency, nodeName };
/// How a change to a row is taken (docs/writing-a-node-type.md#change-marks): the node restarts, for the reason the row
/// gives (Row::restarts), or the type's change version applies it while the node runs (Row::inPlace).
enum class Change { restart, inPlace };
/// What a control may offer for a text row (docs/writing-a-node-type.md#the-type-schema): the whole set of values it takes
/// (`values`, a select), an ECMAScript pattern every good value matches (`pattern`), and the most bytes a good value
/// has (`maxLength`, 0 for none). Hints only: the row's parse function stays the judge, and the all-type tests hold
/// each hint to it (every value parses; every valid setting matches the pattern and fits).
struct TextHints {
	std::span<const std::string_view> values = {};
	std::string_view pattern = {};
	std::uint32_t maxLength = 0;
};
/// How a control maps its travel to a decimal row's value (docs/writing-a-node-type.md#decimal-rows): evenly, on the
/// value's logarithm (a frequency), or as a level in dB.
enum class Taper { linear, log, db };
/// What a control needs to draw a decimal row (docs/writing-a-node-type.md#decimal-rows): the places it shows, its
/// taper, and whether the row's low end means silence (a control shows -inf there). Hints only: the walk judges by the
/// range.
struct DecimalHints {
	std::uint8_t decimals = 2;
	Taper taper = Taper::linear;
	bool minIsSilence = false;
};
} // namespace setting
/// One whole-number rule. A good value is a JSON integer (a float, a bool and a string are not) in [low, high]; an integer
/// above INT64_MAX is above every range. Otherwise "<key>: must be a whole number[ of <unit>], <low>-<high>".
struct WholeNumberRule {
	std::uint32_t low;
	std::uint32_t high;
	std::string_view unit;
};
/// One decimal rule. A good value is a JSON number (an integer is taken), finite, in [low, high]. Otherwise "<key>:
/// must be a number[ of <unit>] from <low> to <high>", the ends in decimalText's form.
struct DecimalRule {
	double low;
	double high;
	std::string_view unit;
	setting::DecimalHints hints = {};
};
/// One rate rule (docs/graph-file.md#rates). A good value is a JSON whole number of hertz (rateOfSetting) in [low, high];
/// in ear.conf it is written with its unit (44.1khz). Otherwise "<key>: must be a rate from <low> to <high> (on the
/// control port, a whole number of Hz)", the ends in rateText's form.
struct RateRule {
	SampleRate low;
	SampleRate high;
};
/// One text rule. A good value is a JSON string `read` accepts; `read` returns the parsed value, empty when refused.
/// Otherwise "<key>: must be <mustBe>".
struct TextRule {
	std::string_view mustBe;
	std::any (*read)(std::string_view text);
	setting::TextHints hints = {};
};
/// One boolean rule. A good value is JSON true or false (not a string, a number or null); in ear.conf it is the bare word
/// true or false. Otherwise "<key>: must be true or false".
struct BooleanRule {};
/// Any one rule, as a row holds it.
using SettingRule = std::variant<WholeNumberRule, TextRule, RateRule, BooleanRule, DecimalRule>;
/// A defaulted row's default, of its rule's kind: a whole number, a decimal or a boolean.
using SettingValue = std::variant<std::uint32_t, double, bool>;
/// One row as the walk reads it. A key given with any value, null included, is given. Anyone can fill one in, but a table
/// lists only typed rows, and a typed row gives its SettingRow to read, never to change.
struct SettingRow {
	std::string_view key;
	bool required; // absent: "<key>: missing"
	SettingRule rule;
	std::optional<SettingValue> byDefault = {};           // what the row reads when absent
	std::string_view onlyWithout = {};                    // Row::onlyWithout's
	setting::Change change = setting::Change::restart;    // the row's change mark: Row::inPlace or Row::restarts
	std::string_view restartReason = {};                  // Row::restarts's: why a change to it restarts the node
	std::optional<setting::DefaultFrom> defaultFrom = {}; // Row::defaultFrom's: where an absent value comes from
	bool shapesPorts = false;                             // Row::shapesPorts's: a change to it may change the ports
	std::string_view help = {};                           // Row::help's: one line for controls
};

namespace setting {
template <class Result> inline constexpr bool isOwningOptional = false;
template <class T> inline constexpr bool isOwningOptional<std::optional<T>> =
	!std::ranges::view<T> && !std::is_pointer_v<T> && std::copy_constructible<T>;
/// What a text row can parse with: invocable with the text, returning an optional of a value it can keep. This refuses a
/// value that is itself a view or a pointer. C++ cannot see inside a struct, so one holding a view of the text passes and
/// dangles: a parse function's value must own what it holds, and only review checks that.
template <auto Parse>
concept TextParser = std::invocable<decltype(Parse), std::string_view> &&
                     isOwningOptional<std::invoke_result_t<decltype(Parse), std::string_view>>;
template <auto Parse> using ParsedBy = typename std::invoke_result_t<decltype(Parse), std::string_view>::value_type;

template <class T> class Rule;
/// The range is not optional; low above high does not compile.
consteval Rule<std::uint32_t> number(std::uint32_t low, std::uint32_t high, std::string_view unit = {});
/// A finite real in [low, high], read as double: a JSON number on the control port (an integer is taken), a number word
/// in ear.conf (-3.5, 0.25, 1e3). A bound not finite, low above high, a log taper's low not above 0, or a dB taper not
/// reaching from -60 dB or lower to 0 dB or higher does not compile.
consteval Rule<double> decimal(double low, double high, std::string_view unit = {}, DecimalHints hints = {});
/// A rate, read as SampleRate: written with its unit in ear.conf, a whole number of hertz on the control port. Low above
/// high does not compile.
consteval Rule<SampleRate> rate(SampleRate low, SampleRate high);
/// JSON true or false, read as bool: the bare word true or false in ear.conf.
consteval Rule<bool> boolean();
/// Parse: std::optional<T>(std::string_view), T not a view. T is what the row reads. An empty mustBe does not compile,
/// nor an empty value among the hints' `values`.
template <auto Parse>
	requires TextParser<Parse>
consteval Rule<ParsedBy<Parse>> text(std::string_view mustBe, TextHints hints = {});

/// A checked rule whose value reads as T. Only number, rate, boolean and text make one, so T is always what the rule's
/// reader gives.
template <class T> class Rule {
public:
	using Value = T;
	constexpr const SettingRule& held() const noexcept { return rule_; }

private:
	consteval explicit Rule(SettingRule rule) : rule_(rule) {}
	friend consteval Rule<std::uint32_t> number(std::uint32_t low, std::uint32_t high, std::string_view unit);
	friend consteval Rule<double> decimal(double low, double high, std::string_view unit, DecimalHints hints);
	friend consteval Rule<SampleRate> rate(SampleRate low, SampleRate high);
	friend consteval Rule<bool> boolean();
	template <auto Parse>
		requires TextParser<Parse>
	friend consteval Rule<ParsedBy<Parse>> text(std::string_view mustBe, TextHints hints);
	SettingRule rule_;
};

/// A row a type reads: required and defaulted rows read as T, an optional row as std::optional<T>. Reads are copies, so
/// one taken from a temporary cannot dangle. Only required, optional and defaulted make one.
template <class T, bool Always> class Row {
public:
	using Value = T;
	using Reads = std::conditional_t<Always, T, std::optional<T>>;
	constexpr const SettingRow& row() const noexcept { return row_; }
	/// Given together with `above`, this key is refused: `<key>: only without "<above's key>"`, before its kind is looked
	/// at. The table refuses, at compile time, any `above` that is not a row above this one. A required row on either side
	/// does not compile: the table would refuse every object, or this row could never be given.
	template <class Other, bool OtherAlways> consteval Row onlyWithout(const Row<Other, OtherAlways>& above) const {
		if (row_.required) throw "a required row cannot be onlyWithout another: no object would pass";
		if (above.row().required) throw "an onlyWithout cannot name a required row: this row could never be given";
		Row limited = *this;
		limited.row_.onlyWithout = above.row().key;
		return limited;
	}
	/// Whether the row has its change mark, inPlace or restarts (docs/writing-a-node-type.md#change-marks): a spec's
	/// every settings row has one, and a call's arguments none.
	constexpr bool marked() const noexcept {
		return row_.change != setting::Change::restart || !row_.restartReason.empty();
	}
	/// A change to this key may apply in place, through the type's change version of configure, while the node keeps
	/// running (docs/writing-a-node-type.md#change-marks). A row has one mark: this or restarts.
	consteval Row inPlace() const {
		if (marked()) throw "a row has one change mark: inPlace or restarts, never both and never twice";
		Row mark = *this;
		mark.row_.change = setting::Change::inPlace;
		return mark;
	}
	/// A change to this key restarts the node, for `why`: one line naming what cannot change while the node runs
	/// ("shairport-sync reads it only at start"), shown beside "restarts" (docs/writing-a-node-type.md#change-marks).
	/// An empty reason does not compile, and a row has one mark: this or inPlace.
	consteval Row restarts(std::string_view why) const {
		if (marked()) throw "a row has one change mark: inPlace or restarts, never both and never twice";
		if (why.empty()) throw "a restart states why: restarts takes a reason, never an empty one";
		Row mark = *this;
		mark.row_.restartReason = why;
		return mark;
	}
	/// A change to this key may change the node's ports: the type schema's `shapes` (docs/writing-a-node-type.md#the-type-schema),
	/// the editor's "changes the ports". The plan never reads it (it compares the ports before and after:
	/// docs/architecture.md#reshapes); the all-type tests hold it to the type's ports(), both ways.
	consteval Row shapesPorts() const {
		Row marked = *this;
		marked.row_.shapesPorts = true;
		return marked;
	}
	/// Where this optional row's value comes from when it is absent, for controls and the type schema
	/// (docs/writing-a-node-type.md#the-type-schema): the type's code still applies it. A required or defaulted row always reads
	/// a value, so marking one does not compile.
	/// One line a control shows beside the row (docs/writing-a-node-type.md#the-type-schema): what an absent optional
	/// row means when it has neither a default nor defaultFrom, or anything mustBe does not say. Empty does not compile.
	consteval Row help(std::string_view line) const {
		if (line.empty()) throw "a row's help is a line, never empty";
		Row marked = *this;
		marked.row_.help = line;
		return marked;
	}
	consteval Row defaultFrom(DefaultFrom from) const {
		if (Always) throw "only an optional row takes defaultFrom: a required or defaulted row always reads a value";
		Row marked = *this;
		marked.row_.defaultFrom = from;
		return marked;
	}

private:
	// A throw in a consteval function is what stops the compile; the text is what the compiler shows.
	consteval Row(std::string_view key, bool required, SettingRule rule,
	              std::optional<SettingValue> byDefault = std::nullopt)
		: row_{key, required, rule, byDefault} {
		if (key.empty()) throw "a setting's key is empty";
		// The framework's key on every node (docs/architecture.md#the-node-lifecycle): never a type's, nor an indexed
		// family's base.
		if (key == "disabled" || key.starts_with("disabled."))
			throw "disabled is the framework's key: no type may declare it";
	}
	template <class V> friend consteval Row<V, true> required(std::string_view key, Rule<V> rule);
	template <class V> friend consteval Row<V, false> optional(std::string_view key, Rule<V> rule);
	template <class V>
	friend consteval Row<V, true> defaulted(std::string_view key, Rule<V> rule, std::type_identity_t<V> byDefault);
	SettingRow row_;
};
template <class T> inline constexpr bool isRow = false;
template <class T, bool Always> inline constexpr bool isRow<Row<T, Always>> = true;

template <auto Parse> std::any readParsed(std::string_view text) {
	auto parsed = std::invoke(Parse, text);
	if (!parsed) return {};
	return std::any(std::move(*parsed));
}

consteval Rule<std::uint32_t> number(std::uint32_t low, std::uint32_t high, std::string_view unit) {
	if (low > high) throw "a whole number's low is above its high";
	return Rule<std::uint32_t>(WholeNumberRule{low, high, unit});
}
consteval bool isFiniteNumber(double value) {
	return value == value && value <= std::numeric_limits<double>::max() &&
	       value >= std::numeric_limits<double>::lowest();
}
consteval Rule<double> decimal(double low, double high, std::string_view unit, DecimalHints hints) {
	if (!isFiniteNumber(low) || !isFiniteNumber(high)) throw "a decimal's bounds are finite numbers";
	if (low > high) throw "a decimal's low is above its high";
	if (hints.taper == Taper::log && !(low > 0)) throw "a log taper's low is above 0: a logarithm of 0 or less is none";
	if (hints.taper == Taper::db && !(low <= -60 && high >= 0))
		throw "a dB taper reaches from -60 dB or lower to 0 dB or higher";
	return Rule<double>(DecimalRule{low, high, unit, hints});
}
consteval Rule<SampleRate> rate(SampleRate low, SampleRate high) {
	if (low > high) throw "a rate's low is above its high";
	return Rule<SampleRate>(RateRule{low, high});
}
consteval Rule<bool> boolean() { return Rule<bool>(BooleanRule{}); }
template <auto Parse>
	requires TextParser<Parse>
consteval Rule<ParsedBy<Parse>> text(std::string_view mustBe, TextHints hints) {
	if (mustBe.empty()) throw "a text setting needs what it must be";
	for (const std::string_view value : hints.values)
		if (value.empty()) throw "a text setting's values are never empty";
	return Rule<ParsedBy<Parse>>(TextRule{mustBe, &readParsed<Parse>, hints});
}
template <class V> consteval Row<V, true> required(std::string_view key, Rule<V> rule) {
	return Row<V, true>(key, true, rule.held());
}
template <class V> consteval Row<V, false> optional(std::string_view key, Rule<V> rule) {
	return Row<V, false>(key, false, rule.held());
}
/// A whole number, decimal or boolean row that reads `byDefault` when absent; a default outside the range does not
/// compile, nor a text or rate row with a default (docs/writing-a-node-type.md#settings-one-table-of-typed-rows). The
/// default is not deduced, so `defaulted("playback", number(0, 64), 0)` takes the int literal as the rule's value.
template <class V>
consteval Row<V, true> defaulted(std::string_view key, Rule<V> rule, std::type_identity_t<V> byDefault) {
	if constexpr (std::same_as<V, std::uint32_t>) {
		const WholeNumberRule range = std::get<WholeNumberRule>(rule.held());
		if (byDefault < range.low || byDefault > range.high) throw "a whole number's byDefault is outside its range";
		return Row<V, true>(key, false, rule.held(), SettingValue(byDefault));
	} else if constexpr (std::same_as<V, double>) {
		const DecimalRule range = std::get<DecimalRule>(rule.held());
		if (!isFiniteNumber(byDefault) || byDefault < range.low || byDefault > range.high)
			throw "a decimal's byDefault is outside its range";
		return Row<V, true>(key, false, rule.held(), SettingValue(byDefault));
	} else if constexpr (std::same_as<V, bool>) {
		return Row<V, true>(key, false, rule.held(), SettingValue(byDefault));
	} else {
		throw "only a whole number, decimal or boolean row takes a default: a text or rate row is required or optional";
	}
}
constexpr SettingDeclaration declarationOf(const SettingRow& row) {
	if (std::holds_alternative<TextRule>(row.rule)) return {row.key, SettingKind::text};
	if (std::holds_alternative<RateRule>(row.rule)) return {row.key, SettingKind::rate};
	if (std::holds_alternative<BooleanRule>(row.rule)) return {row.key, SettingKind::boolean};
	if (std::holds_alternative<DecimalRule>(row.rule)) return {row.key, SettingKind::decimal};
	return {row.key, SettingKind::wholeNumber};
}
} // namespace setting

template <const auto&... Rows>
	requires(setting::isRow<std::remove_cvref_t<decltype(Rows)>> && ...)
class SettingsTable;
namespace spec_detail {
struct ArgsAccess; // action_spec.hpp: a call's checked arguments, made from what its table's walk made
} // namespace spec_detail

namespace linkage {
template <const auto& Object> consteval std::string_view spelling() { return __PRETTY_FUNCTION__; }
/// Whether `Object` is declared outside any unnamed namespace (docs/writing-a-node-type.md#the-spec). GCC 14
/// gives a template specialised on a reference to an object external linkage even when the object is in an unnamed
/// namespace, so two files' `(anonymous namespace)::pathRow` name one SettingsTable or one Io, and the
/// linker keeps one file's instantiation for both, silently. Every object a table, a type or a spec is keyed on is
/// therefore declared in a named namespace (a type's own: ear::nodes::<type>); this refuses one that is not. It reads
/// the compiler's own spelling of the argument, GCC's "{anonymous}" or Clang's "(anonymous namespace)".
template <const auto& Object> inline constexpr bool named =
	spelling<Object>().find("{anonymous}") == std::string_view::npos &&
	spelling<Object>().find("(anonymous namespace)") == std::string_view::npos;
} // namespace linkage

/// What the walk made: one slot per row. No public constructor and no public read: only the walk makes one, and only
/// Checked reads it. It owns its values, so it borrows neither the settings nor the table.
class CheckedSettings {
private:
	explicit CheckedSettings(std::vector<std::any> slots) noexcept : slots_(std::move(slots)) {}
	friend class AnySettingsTable;
	template <const auto&... Rows>
		requires(setting::isRow<std::remove_cvref_t<decltype(Rows)>> && ...)
	friend class SettingsTable;
	friend struct spec_detail::ArgsAccess;
	std::vector<std::any> slots_;
};

/// A table without its rows' types: all the walk needs, so the walk is not a template. Its constructor is consteval, so
/// run-time rows cannot reach the walk.
class AnySettingsTable {
public:
	constexpr std::string_view name() const noexcept { return name_; }
	/// NodeType::settingKinds(): every key, each once, in row order; the same span every call.
	constexpr std::span<const SettingDeclaration> kinds() const noexcept { return kinds_; }
	/// Every row, in the walk's order: what the type schema is made from (NodeType::schema).
	constexpr std::span<const SettingRow* const> rows() const noexcept { return rows_; }
	/// The kind of `key`'s row (NodeType::kindOf); nothing for a key the table lacks.
	constexpr std::optional<SettingKind> kindOf(std::string_view key) const noexcept {
		for (const SettingDeclaration& declared : kinds_)
			if (declared.key == key) return declared.kind;
		return std::nullopt;
	}
	/// Whether the row of `key` is marked inPlace (Row::inPlace); false for a key the table lacks.
	constexpr bool changesInPlace(std::string_view key) const noexcept {
		for (const SettingRow* row : rows_)
			if (row->key == key) return row->change == setting::Change::inPlace;
		return false;
	}
	/// A refusal's text, and the key it names (empty for "settings: must be an object"): InvalidSettings's `key`
	/// (docs/control-port.md#error-codes), and an action's invalid-args.
	struct Refusal {
		std::string text;
		std::string key;
	};
	/// A call's arguments walked as settings are (docs/writing-a-node-type.md#actions): what NodeHost hands the call's
	/// handler, or the refusal.
	std::expected<CheckedSettings, Refusal> checkArgs(const NodeSettings& args) const { return walk(args); }

protected:
	/// Refuses at compile time: an empty name, a key given twice, an onlyWithout that names no row above its own.
	consteval AnySettingsTable(std::string_view name, std::span<const SettingRow* const> rows,
	                           std::span<const SettingDeclaration> kinds)
		: name_(name), rows_(rows), kinds_(kinds) {
		if (name.empty()) throw "a table's name is empty";
		for (std::size_t at = 0; at < rows.size(); ++at) {
			bool aboveFound = false;
			for (std::size_t earlier = 0; earlier < at; ++earlier) {
				if (rows[earlier]->key == rows[at]->key) throw "a setting's key is given twice";
				if (rows[earlier]->key == rows[at]->onlyWithout) aboveFound = true;
			}
			if (!rows[at]->onlyWithout.empty() && !aboveFound) throw "an onlyWithout must name a row above its own";
		}
	}
	/// The first failure is the refusal's text, in this order:
	/// 1. Not an object: "settings: must be an object", with no rows; else that and ` with "a", "b" and "c"` naming the
	///    required keys, or when none is required ` with optional "a", "b" and "c"` naming every key. With no rows, null
	///    is taken as {}.
	/// 2. An undeclared key, the first in the object's own (sorted) order: "unknown key <key>", with no rows followed by
	///    " (<name> takes no settings)".
	/// 3. Each row in order: absent (next row, or missing when required), then onlyWithout, then the kind and the range
	///    or meaning.
	std::expected<CheckedSettings, Refusal> walk(const NodeSettings& settings) const;

private:
	friend class NodeType;
	std::string_view name_;
	std::span<const SettingRow* const> rows_;
	std::span<const SettingDeclaration> kinds_;
};

/// A type's table, declared once: constexpr SettingsTable<addressRow, portRow> table{"rtp"}; Rows are checked in this
/// order, which is also settingKinds()'s. With no rows: SettingsTable<>. Only setting's typed rows can be listed.
template <const auto&... Rows>
	requires(setting::isRow<std::remove_cvref_t<decltype(Rows)>> && ...)
class SettingsTable final : public AnySettingsTable {
	static_assert(
		(linkage::named<Rows> && ...),
		"a table's rows are declared in a named namespace, never an unnamed one: GCC 14 would give two files' "
		"same-named rows one table");

public:
	consteval explicit SettingsTable(std::string_view name) : AnySettingsTable(name, rows_, kinds_) {}
	/// Whether every row has its change mark, as a spec's settings must (TypeSpec), and whether any has one, as a call's
	/// arguments must not (action::call): docs/writing-a-node-type.md#change-marks.
	static constexpr bool everyRowMarked = (Rows.marked() && ...);
	static constexpr bool anyRowMarked = (Rows.marked() || ...);
	/// Settings this table passed. A row not listed in this table cannot be read: get's constraint fails at compile time.
	class Checked {
	public:
		template <const auto& Row>
			requires((static_cast<const void*>(&Row) == static_cast<const void*>(&Rows)) || ...)
		[[nodiscard]] typename std::remove_cvref_t<decltype(Row)>::Reads get() const {
			using Read = std::remove_cvref_t<decltype(Row)>;
			constexpr auto slot = std::ranges::find(rows_, &Row.row()) - rows_.begin();
			const auto* value = std::any_cast<typename Read::Value>(&settings_.slots_[static_cast<std::size_t>(slot)]);
			if constexpr (std::same_as<typename Read::Reads, typename Read::Value>) {
				return *value; // the walk filled it: a required row was given, a defaulted one has its default
			} else {
				return value != nullptr ? typename Read::Reads(*value) : std::nullopt;
			}
		}

	private:
		explicit Checked(CheckedSettings settings) noexcept : settings_(std::move(settings)) {}
		friend SettingsTable;
		template <const auto& Spec, class Type> friend class spec_detail::SpecNodeType;
		friend struct spec_detail::ArgsAccess;
		CheckedSettings settings_;
	};
	/// The walk with no node type around it, for tests; production goes through NodeType::parse.
	std::expected<Checked, std::string> check(const NodeSettings& settings) const {
		auto walked = walk(settings);
		if (!walked) return std::unexpected(std::move(walked.error().text));
		return Checked(std::move(*walked));
	}

private:
	static constexpr std::array<const SettingRow*, sizeof...(Rows)> rows_{&Rows.row()...};
	static constexpr std::array<SettingDeclaration, sizeof...(Rows)> kinds_{setting::declarationOf(Rows.row())...};
};
} // namespace ear
