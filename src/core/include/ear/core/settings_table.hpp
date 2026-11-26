// One node type's settings, each key declared once, as typed rows in one table. The table checks the settings object in
// one fixed order and makes every refusal's text from the row; NodeType's name(), settingKinds() and parse() all answer
// from it and cannot be overridden, so the graph file's reader cannot disagree with parse. A type's class gets typed
// values (CheckedOf<spec>, lifecycle.hpp), never the raw object, and its `check` holds any rule about them.
// Pure: nlohmann/json only.
#pragma once
#include <algorithm>
#include <any>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
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
/// gives (Row::restarts), the type's change version applies it while the node runs (Row::inPlace), or it moves a slot
/// the code reads on the realtime thread (Row::live, docs/writing-a-node-type.md#live-rows).
enum class Change { restart, inPlace, live };
/// A live row's ramp (docs/writing-a-node-type.md#live-rows): the code's view moves to a new value over `ms`.
struct Ramp {
	std::uint32_t ms;
};
/// A ramp over `ms`, 1 to 1000 ms; ramp(0) does not compile: a row with no ramp is stepped (setting::step).
consteval Ramp ramp(std::uint32_t ms) {
	if (ms == 0) throw "a ramp takes 1 to 1000 ms: a row with none is live(setting::step)";
	if (ms > 1000) throw "a ramp takes 1 to 1000 ms";
	return Ramp{ms};
}
/// A live row's step: the code sees a new value from the next block whole, and makes its own crossfade.
struct Step {};
inline constexpr Step step{};
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
/// One choice rule (docs/writing-a-node-type.md#choice-rows): a closed set of names, each read as its enumerator. A good
/// value is a JSON string equal to one of the names. Otherwise "<key>: must be one of "<a>", "<b>" and "<c>"". The
/// names are held in the rule itself, so a row made from a braced list owns them; a name's index is its place here.
struct ChoiceRule {
	/// The most names one choice holds.
	static constexpr std::size_t capacity = 32;
	std::array<std::string_view, capacity> names{};
	/// Each name's enumerator, as its underlying value.
	std::array<std::int64_t, capacity> values{};
	std::uint32_t count = 0;
	constexpr std::span<const std::string_view> taken() const noexcept { return {names.data(), count}; }
};
/// Any one rule, as a row holds it.
using SettingRule = std::variant<WholeNumberRule, TextRule, RateRule, BooleanRule, DecimalRule, ChoiceRule>;
/// A choice's value without its enumeration: its name's index in its rule (ChoiceRule::names).
struct ChoiceIndex {
	std::uint32_t index;
	friend constexpr bool operator==(ChoiceIndex, ChoiceIndex) = default;
};
/// A defaulted row's default, of its rule's kind: a whole number, a decimal, a boolean or a choice's index.
using SettingValue = std::variant<std::uint32_t, double, bool, ChoiceIndex>;
/// One row as the walk reads it. A key given with any value, null included, is given. Anyone can fill one in, but a table
/// lists only typed rows, and a typed row gives its SettingRow to read, never to change.
struct SettingRow {
	std::string_view key;
	bool required; // absent: "<key>: missing"
	SettingRule rule;
	std::optional<SettingValue> byDefault = {};           // what the row reads when absent
	std::string_view onlyWithout = {};                    // Row::onlyWithout's
	setting::Change change = setting::Change::restart;    // the row's change mark: Row::live, inPlace or restarts
	std::string_view restartReason = {};                  // Row::restarts's: why a change to it restarts the node
	std::optional<setting::DefaultFrom> defaultFrom = {}; // Row::defaultFrom's: where an absent value comes from
	bool shapesPorts = false;                             // Row::shapesPorts's: a change to it may change the ports
	std::string_view help = {};                           // Row::help's: one line for controls
	/// An indexed family's (setting::indexed, docs/writing-a-node-type.md#indexed-families): the key of its count row,
	/// and that row's high, the most members it takes. Empty and 0 for a plain row.
	std::string_view indexedBy = {};
	std::uint32_t maxMembers = 0;
	/// A live row's transition (Row::live): its ramp's ms, or 0 for a step. 0 for a row that is not live.
	std::uint32_t rampMs = 0;
};
namespace setting {
/// A family member's number in `key` (docs/writing-a-node-type.md#indexed-families): `key` is `<family>.<n>`, n
/// written in decimal from 1 with no sign and no leading zero; nothing for any other spelling. A number too large for
/// 32 bits is the largest there is, so it is above every family's maximum.
constexpr std::optional<std::uint32_t> memberNumber(std::string_view key, std::string_view family) noexcept {
	if (key.size() < family.size() + 2 || !key.starts_with(family) || key[family.size()] != '.') return std::nullopt;
	const std::string_view digits = key.substr(family.size() + 1);
	if (digits.front() < '1' || digits.front() > '9') return std::nullopt;
	std::uint64_t number = 0;
	for (const char digit : digits) {
		if (digit < '0' || digit > '9') return std::nullopt;
		number = std::min<std::uint64_t>(number * 10 + static_cast<std::uint64_t>(digit - '0'),
		                                 std::numeric_limits<std::uint32_t>::max());
	}
	return static_cast<std::uint32_t>(number);
}
/// Whether a row is live (Row::live), and how many live members it is: a family its maximum, a plain row one, any
/// other row none (docs/writing-a-node-type.md#live-rows). The one count the table, the channels and NodeHost share.
constexpr bool isLive(const SettingRow& row) noexcept { return row.change == Change::live; }
constexpr std::uint32_t liveMembers(const SettingRow& row) noexcept {
	if (!isLive(row)) return 0;
	return row.maxMembers > 0 ? row.maxMembers : 1;
}
/// The most live members one type has: its realtime channels hold one slot each.
inline constexpr std::uint32_t mostLiveMembers = 256;
} // namespace setting

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
template <class T> class IndexedRow;
/// A pair a choice is made of: a name and the enumerator it reads as.
template <class E> using Named = std::pair<std::string_view, E>;
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
/// A closed set of names read as E (docs/writing-a-node-type.md#choice-rows): a JSON string equal to one of them on the
/// control port, a word or a quoted string in ear.conf; a name's index is its place here. An empty set, more than
/// ChoiceRule::capacity names, a name repeated, an enumerator named twice, or a name that is empty or holds a control
/// character does not compile.
template <class E>
	requires std::is_enum_v<E>
consteval Rule<E> choice(std::initializer_list<Named<E>> names);
/// A choice's value read back as its enumerator, from the row's own rule.
template <class E> constexpr E choiceValue(const SettingRow& row, ChoiceIndex index) noexcept {
	return static_cast<E>(static_cast<std::underlying_type_t<E>>(std::get<ChoiceRule>(row.rule).values[index.index]));
}
/// Whether a row reads by a choice rule: by the rule, never the value's type, since a text row may read an enumeration.
constexpr bool isChoice(const SettingRow& row) noexcept { return std::holds_alternative<ChoiceRule>(row.rule); }
/// A live member's value as its slot holds it (docs/architecture.md#the-realtime-channels): a decimal's 64 bits, a whole
/// number, a boolean 0 or 1, a choice's index.
template <class V> constexpr std::uint64_t liveBits(const V& value) noexcept {
	if constexpr (std::same_as<V, double>) return std::bit_cast<std::uint64_t>(value);
	else if constexpr (std::same_as<V, bool>) return value ? 1 : 0;
	else if constexpr (std::same_as<V, ChoiceIndex>) return value.index;
	else return static_cast<std::uint64_t>(value);
}
/// A slot's bits read back as `row`'s value V: liveBits's inverse, a choice's index through its rule.
template <class V> constexpr V liveValueOf(const SettingRow& row, std::uint64_t bits) noexcept {
	if constexpr (std::same_as<V, double>) return std::bit_cast<double>(bits);
	else if constexpr (std::same_as<V, bool>) return bits != 0;
	else if constexpr (std::is_enum_v<V>) return choiceValue<V>(row, ChoiceIndex{static_cast<std::uint32_t>(bits)});
	else return static_cast<V>(bits);
}
/// What the walk keeps for a value read as V by a row: a choice's its index, every other kind the value itself.
template <class V, bool Choice> using SlotOf = std::conditional_t<Choice, ChoiceIndex, V>;
/// Parse: std::optional<T>(std::string_view), T not a view. T is what the row reads. An empty mustBe does not compile,
/// nor an empty value among the hints' `values`.
template <auto Parse>
	requires TextParser<Parse>
consteval Rule<ParsedBy<Parse>> text(std::string_view mustBe, TextHints hints = {});

/// A checked rule whose value reads as T. Only number, decimal, rate, boolean, choice and text make one, so T is always
/// what the rule's reader gives.
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
	template <class E>
		requires std::is_enum_v<E>
	friend consteval Rule<E> choice(std::initializer_list<Named<E>> names);
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
	/// running (docs/writing-a-node-type.md#change-marks). A row has one mark: this, live or restarts.
	consteval Row inPlace() const {
		if (marked()) throw "a row has one change mark: live, inPlace or restarts, never two and never twice";
		Row mark = *this;
		mark.row_.change = setting::Change::inPlace;
		return mark;
	}
	/// A change to this key moves a slot the code reads through io.param, ramped over the ramp's ms
	/// (docs/writing-a-node-type.md#live-rows). Only a row that always reads a value (required or defaulted) of a
	/// decimal or a boolean (a weight from 0 to 1) is ramped; a live row never shapes the ports.
	consteval Row live(Ramp ramp) const {
		liveMark();
		if (!std::holds_alternative<DecimalRule>(row_.rule) && !std::holds_alternative<BooleanRule>(row_.rule))
			throw "a ramp needs a decimal or a boolean row: a whole number or a choice is live(setting::step)";
		Row mark = *this;
		mark.row_.change = setting::Change::live;
		mark.row_.rampMs = ramp.ms;
		return mark;
	}
	/// A change to this key moves a slot the code reads through io.param, whole from the next block
	/// (docs/writing-a-node-type.md#live-rows): a decimal, whole number, boolean or choice row that always reads a
	/// value.
	consteval Row live(Step) const {
		liveMark();
		Row mark = *this;
		mark.row_.change = setting::Change::live;
		return mark;
	}
	/// A change to this key restarts the node, for `why`: one line naming what cannot change while the node runs
	/// ("shairport-sync reads it only at start"), shown beside "restarts" (docs/writing-a-node-type.md#change-marks).
	/// An empty reason does not compile, and a row has one mark: this, live or inPlace.
	consteval Row restarts(std::string_view why) const {
		if (marked()) throw "a row has one change mark: live, inPlace or restarts, never two and never twice";
		if (why.empty()) throw "a restart states why: restarts takes a reason, never an empty one";
		Row mark = *this;
		mark.row_.restartReason = why;
		return mark;
	}
	/// A change to this key may change the node's ports: the type schema's `shapes` (docs/writing-a-node-type.md#the-type-schema),
	/// the editor's "changes the ports". The plan never reads it (it compares the ports before and after:
	/// docs/architecture.md#reshapes); the all-type tests hold it to the type's ports(), both ways.
	consteval Row shapesPorts() const {
		if (row_.change == setting::Change::live)
			throw "a live row never shapes the ports: ports() cannot read it (docs/writing-a-node-type.md#live-rows)";
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
	/// What every live mark checks first: one mark per row, a row that always reads a value, a kind that can be live,
	/// and no shapesPorts.
	consteval void liveMark() const {
		if (marked()) throw "a row has one change mark: live, inPlace or restarts, never two and never twice";
		if (std::holds_alternative<TextRule>(row_.rule) || std::holds_alternative<RateRule>(row_.rule))
			throw "a live row is a decimal, whole number, boolean or choice: a text or a rate names what is never live";
		if (!Always) throw "a live row always reads a value: required, defaulted or a family, never optional";
		if (row_.shapesPorts)
			throw "a live row never shapes the ports: ports() cannot read it (docs/writing-a-node-type.md#live-rows)";
	}
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
template <class V, class C, bool A> consteval IndexedRow<V> indexed(std::string_view key, const Row<C, A>& count,
                                                                    Rule<V> rule, std::type_identity_t<V> byDefault);
/// An indexed family (docs/writing-a-node-type.md#indexed-families): one row declaring the members `<key>.1` to
/// `<key>.<n>`, n its count row's value, each read as T and as the family's default when absent. Members up to the
/// count row's high are taken; those above its value are kept, inactive. Only indexed makes one.
template <class T> class IndexedRow {
public:
	using Value = T;
	constexpr const SettingRow& row() const noexcept { return row_; }
	/// As Row's: the family's mark is every member's.
	constexpr bool marked() const noexcept {
		return row_.change != setting::Change::restart || !row_.restartReason.empty();
	}
	consteval IndexedRow inPlace() const {
		if (marked()) throw "a row has one change mark: live, inPlace or restarts, never two and never twice";
		IndexedRow mark = *this;
		mark.row_.change = setting::Change::inPlace;
		return mark;
	}
	/// As Row's: every member a slot, made for the family's maximum, active or not.
	consteval IndexedRow live(Ramp ramp) const {
		if (marked()) throw "a row has one change mark: live, inPlace or restarts, never two and never twice";
		if (!std::holds_alternative<DecimalRule>(row_.rule) && !std::holds_alternative<BooleanRule>(row_.rule))
			throw "a ramp needs a decimal or a boolean row: a whole number or a choice is live(setting::step)";
		IndexedRow mark = *this;
		mark.row_.change = setting::Change::live;
		mark.row_.rampMs = ramp.ms;
		return mark;
	}
	consteval IndexedRow live(Step) const {
		if (marked()) throw "a row has one change mark: live, inPlace or restarts, never two and never twice";
		IndexedRow mark = *this;
		mark.row_.change = setting::Change::live;
		return mark;
	}
	consteval IndexedRow restarts(std::string_view why) const {
		if (marked()) throw "a row has one change mark: live, inPlace or restarts, never two and never twice";
		if (why.empty()) throw "a restart states why: restarts takes a reason, never an empty one";
		IndexedRow mark = *this;
		mark.row_.restartReason = why;
		return mark;
	}
	consteval IndexedRow help(std::string_view line) const {
		if (line.empty()) throw "a row's help is a line, never empty";
		IndexedRow marked = *this;
		marked.row_.help = line;
		return marked;
	}

private:
	consteval explicit IndexedRow(SettingRow row) : row_(row) {}
	template <class V, class C, bool A> friend consteval IndexedRow<V>
	indexed(std::string_view key, const Row<C, A>& count, Rule<V> rule, std::type_identity_t<V> byDefault);
	SettingRow row_;
};
/// The rows a table lists: plain rows and indexed families.
template <class T> inline constexpr bool isRow = false;
template <class T, bool Always> inline constexpr bool isRow<Row<T, Always>> = true;
template <class T> inline constexpr bool isRow<IndexedRow<T>> = true;
template <class T> inline constexpr bool isIndexed = false;
template <class T> inline constexpr bool isIndexed<IndexedRow<T>> = true;

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
/// A name ear.conf can hold as a word or a quoted string, as a type author would write it: not empty, and no control
/// character (U+0000 to U+001F, U+007F).
consteval bool writableName(std::string_view name) {
	if (name.empty()) return false;
	for (const char c : name)
		if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7F) return false;
	return true;
}
template <class E>
	requires std::is_enum_v<E>
consteval Rule<E> choice(std::initializer_list<Named<E>> names) {
	if (names.size() == 0) throw "a choice names one value or more: an empty set takes nothing";
	if (names.size() > ChoiceRule::capacity) throw "a choice holds at most 32 names";
	ChoiceRule rule;
	for (const auto& [name, value] : names) {
		if (!writableName(name))
			throw "a choice's name is a word or a string ear.conf can hold: never empty, no control character";
		const auto underlying = static_cast<std::int64_t>(std::to_underlying(value));
		for (std::uint32_t earlier = 0; earlier < rule.count; ++earlier) {
			if (rule.names[earlier] == name) throw "a choice names a value twice";
			if (rule.values[earlier] == underlying)
				throw "a choice names one enumerator twice: each name reads as its own value";
		}
		rule.names[rule.count] = name;
		rule.values[rule.count] = underlying;
		++rule.count;
	}
	return Rule<E>(rule);
}
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
/// A whole number, decimal, boolean or choice row that reads `byDefault` when absent; a default outside the range, or
/// an enumerator the choice does not name, does not compile, nor a text or rate row with a default
/// (docs/writing-a-node-type.md#settings-one-table-of-typed-rows). The default is not deduced, so
/// `defaulted("playback", number(0, 64), 0)` takes the int literal as the rule's value.
template <class V>
consteval Row<V, true> defaulted(std::string_view key, Rule<V> rule, std::type_identity_t<V> byDefault) {
	// A text rule's parse may return any value type, a bool or a whole number among them, so the kind is read from the
	// rule itself, never from V.
	if (std::holds_alternative<TextRule>(rule.held()) || std::holds_alternative<RateRule>(rule.held()))
		throw "only a whole number, decimal, boolean or choice row takes a default: a text or rate row is required or "
			  "optional";
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
	} else if constexpr (std::is_enum_v<V>) {
		if (!std::holds_alternative<ChoiceRule>(rule.held()))
			throw "only a whole number, decimal, boolean or choice row takes a default";
		const ChoiceRule& set = std::get<ChoiceRule>(rule.held());
		const auto underlying = static_cast<std::int64_t>(std::to_underlying(byDefault));
		for (std::uint32_t at = 0; at < set.count; ++at)
			if (set.values[at] == underlying)
				return Row<V, true>(key, false, rule.held(), SettingValue(ChoiceIndex{at}));
		throw "a choice's byDefault is an enumerator it names";
	} else {
		throw "only a whole number, decimal, boolean or choice row takes a default: a text or rate row is required or "
			  "optional";
	}
}
/// A family `<key>.1` .. `<key>.<n>` (docs/writing-a-node-type.md#indexed-families): n the value of `count`, a required
/// or defaulted whole-number row above it in its table; each member reads `byDefault` when absent, which is checked as
/// defaulted's is. A count row of another kind, an optional one, or one whose high is 0 does not compile.
template <class V, class C, bool A> consteval IndexedRow<V> indexed(std::string_view key, const Row<C, A>& count,
                                                                    Rule<V> rule, std::type_identity_t<V> byDefault) {
	if constexpr (!std::same_as<C, std::uint32_t>) {
		throw "a family's count row is a whole number";
	} else {
		if (!A) throw "a family's count row always reads a value: required or defaulted, never optional";
		const auto* range = std::get_if<WholeNumberRule>(&count.row().rule);
		if (range == nullptr) throw "a family's count row is a whole number";
		if (range->high == 0) throw "a family's count row reaches 1 or more: a family of no member is none";
		SettingRow row = defaulted(key, rule, byDefault).row();
		row.indexedBy = count.row().key;
		row.maxMembers = range->high;
		return IndexedRow<V>(row);
	}
}
constexpr SettingDeclaration declarationOf(const SettingRow& row) {
	if (std::holds_alternative<TextRule>(row.rule)) return {row.key, SettingKind::text};
	if (std::holds_alternative<RateRule>(row.rule)) return {row.key, SettingKind::rate};
	if (std::holds_alternative<BooleanRule>(row.rule)) return {row.key, SettingKind::boolean};
	if (std::holds_alternative<DecimalRule>(row.rule)) return {row.key, SettingKind::decimal};
	if (std::holds_alternative<ChoiceRule>(row.rule)) return {row.key, SettingKind::choice};
	return {row.key, SettingKind::wholeNumber};
}
} // namespace setting

template <const auto&... Rows>
	requires(setting::isRow<std::remove_cvref_t<decltype(Rows)>> && ...)
class SettingsTable;
namespace spec_detail {
struct ArgsAccess; // action_spec.hpp: a call's checked arguments, made from what its table's walk made
template <const auto& Spec, class Type> class SpecConfig;   // lifecycle.hpp
template <const auto& Spec, class Type> class SpecInstance; // lifecycle.hpp
} // namespace spec_detail
template <const auto& Spec> class CreateOf; // lifecycle.hpp
template <const auto& Spec> class ChangeOf; // lifecycle.hpp

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

/// A family's slot in CheckedSettings: its given members, (number from 1, value), in number order; a choice's value is
/// its ChoiceIndex (setting::SlotOf).
template <class V> using FamilyValues = std::vector<std::pair<std::uint32_t, V>>;
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
	friend class LiveBinding;
	std::vector<std::any> slots_;
	// The node's live values (docs/writing-a-node-type.md#live-rows) while a hook of its type runs on the main loop
	// (LiveBinding), every live member's slot bits in order; null otherwise, when liveValue reads what was parsed. The
	// main loop alone sets and reads it, so a config other threads share is never raced on it.
	mutable const std::uint64_t* live_ = nullptr;
};
/// For one call of a type's hook on the main loop: `settings` read their live rows from `live` (the node's live values,
/// docs/architecture.md#live-values), then from what was parsed again. Empty `live` binds nothing.
class LiveBinding {
public:
	LiveBinding(const CheckedSettings& settings, std::span<const std::uint64_t> live) noexcept
		: settings_(&settings), before_(settings.live_) {
		if (!live.empty()) settings.live_ = live.data();
	}
	LiveBinding(const LiveBinding&) = delete;
	LiveBinding& operator=(const LiveBinding&) = delete;
	~LiveBinding() { settings_->live_ = before_; }

private:
	const CheckedSettings* settings_;
	const std::uint64_t* before_;
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
	/// The row a key is read by: a plain row's own key (member 0), or a family's member `<family>.<n>` with n from 1 to
	/// the family's maximum (docs/writing-a-node-type.md#indexed-families). The one lookup of a key: the walk, kindOf
	/// and changeOf all go through it.
	struct RowOf {
		const SettingRow* row;
		std::uint32_t member; // 0 for a plain row; a family member's number, from 1
	};
	constexpr std::optional<RowOf> rowOf(std::string_view key) const noexcept {
		for (const SettingRow* row : rows_) {
			if (row->maxMembers == 0) {
				if (row->key == key) return RowOf{row, 0};
			} else if (const auto member = setting::memberNumber(key, row->key)) {
				if (*member <= row->maxMembers) return RowOf{row, *member};
			}
		}
		return std::nullopt;
	}
	/// The kind of `key`'s row (NodeType::kindOf), a family member's by its family; nothing for a key the table lacks.
	constexpr std::optional<SettingKind> kindOf(std::string_view key) const noexcept {
		if (const auto found = rowOf(key)) return setting::declarationOf(*found->row).kind;
		return std::nullopt;
	}
	/// The change mark of `key`'s row, a family member's by its family (docs/writing-a-node-type.md#change-marks);
	/// restart for a key the table lacks.
	constexpr setting::Change changeOf(std::string_view key) const noexcept {
		if (const auto found = rowOf(key)) return found->row->change;
		return setting::Change::restart;
	}
	/// Where `key`'s slot sits among the type's live members (docs/writing-a-node-type.md#live-rows): live rows in the
	/// walk's order, a family's members 1 to its maximum in order; nothing for a key that is not a live member.
	constexpr std::optional<std::uint32_t> liveIndexOf(std::string_view key) const noexcept {
		const auto found = rowOf(key);
		if (!found || !setting::isLive(*found->row)) return std::nullopt;
		std::uint32_t at = 0;
		for (const SettingRow* row : rows_) {
			if (row == found->row) return at + (found->member > 0 ? found->member - 1 : 0);
			at += setting::liveMembers(*row);
		}
		return std::nullopt;
	}
	/// How many live members the type has, each family at its maximum: its channels' slots.
	constexpr std::uint32_t liveCount() const noexcept {
		std::uint32_t count = 0;
		for (const SettingRow* row : rows_) count += setting::liveMembers(*row);
		return count;
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
	/// Refuses at compile time: an empty name, a key given twice, an onlyWithout that names no row above its own, a
	/// family whose count row is not the row above it that indexed was given, and a plain row whose key is a family's
	/// member's spelling.
	consteval AnySettingsTable(std::string_view name, std::span<const SettingRow* const> rows,
	                           std::span<const SettingDeclaration> kinds)
		: name_(name), rows_(rows), kinds_(kinds) {
		if (name.empty()) throw "a table's name is empty";
		for (std::size_t at = 0; at < rows.size(); ++at) {
			bool aboveFound = false;
			bool countFound = false;
			for (std::size_t earlier = 0; earlier < at; ++earlier) {
				if (rows[earlier]->key == rows[at]->key) throw "a setting's key is given twice";
				if (rows[earlier]->key == rows[at]->onlyWithout) aboveFound = true;
				const auto* count = std::get_if<WholeNumberRule>(&rows[earlier]->rule);
				if (rows[at]->maxMembers > 0 && rows[earlier]->key == rows[at]->indexedBy &&
				    rows[earlier]->maxMembers == 0 && count != nullptr && count->high == rows[at]->maxMembers &&
				    (rows[earlier]->required || rows[earlier]->byDefault))
					countFound = true;
			}
			if (!rows[at]->onlyWithout.empty() && !aboveFound) throw "an onlyWithout must name a row above its own";
			if (rows[at]->maxMembers > 0 && !countFound) throw "a family's count row is a row above it in its table";
			for (const SettingRow* family : rows) {
				if (family->maxMembers == 0 || rows[at]->maxMembers > 0) continue;
				if (setting::memberNumber(rows[at]->key, family->key))
					throw "a row's key is a family member's spelling: <family>.<n> belongs to the family";
			}
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
	/// Whether any row is an indexed family, as a call's arguments must not have (action::call).
	static constexpr bool anyRowIndexed = (setting::isIndexed<std::remove_cvref_t<decltype(Rows)>> || ...);
	/// The rows, and the live members (docs/writing-a-node-type.md#live-rows): each family counted at its maximum, so no
	/// count change ever resizes the slots. Over setting::mostLiveMembers does not compile.
	static constexpr std::size_t rowCount = sizeof...(Rows);
	static constexpr std::uint32_t liveMemberCount = (setting::liveMembers(Rows.row()) + ... + 0u);
	static_assert(liveMemberCount <= setting::mostLiveMembers,
	              "a type has at most 256 live members, each family counted at its maximum");
	/// Whether `Row` is one of these rows.
	template <const auto& Row> static constexpr bool listed =
		((static_cast<const void*>(&Row) == static_cast<const void*>(&Rows)) || ...);
	/// Where `Row` sits among the rows, and where its first live member sits among the live members.
	template <const auto& Row> static constexpr std::size_t rowIndexOf = [] {
		constexpr std::array<const void*, sizeof...(Rows)> all{static_cast<const void*>(&Rows)...};
		return static_cast<std::size_t>(std::ranges::find(all, static_cast<const void*>(&Row)) - all.begin());
	}();
	template <const auto& Row> static constexpr std::uint32_t liveFirst = [] {
		constexpr std::array<std::uint32_t, sizeof...(Rows)> members{setting::liveMembers(Rows.row())...};
		std::uint32_t at = 0;
		for (std::size_t row = 0; row < rowIndexOf<Row>; ++row) at += members[row];
		return at;
	}();
	/// Where `row`'s first live member sits among the live members.
	static constexpr std::uint32_t liveAt(const SettingRow& row) noexcept {
		std::uint32_t at = 0;
		for (const SettingRow* one : rows_) {
			if (one == &row) break;
			at += setting::liveMembers(*one);
		}
		return at;
	}
	/// A family's count row.
	template <const auto& Family> static constexpr const SettingRow& countRowOf = *[] {
		constexpr std::array<const SettingRow*, sizeof...(Rows)> all{&Rows.row()...};
		return *std::ranges::find(all, Family.row().indexedBy, &SettingRow::key);
	}();
	/// Settings this table passed. A row not listed in this table cannot be read: get's constraint fails at compile time.
	class Checked {
		template <const auto& Row> static constexpr bool family =
			setting::isIndexed<std::remove_cvref_t<decltype(Row)>>;
		template <const auto& Row> static constexpr std::size_t slotOf() { return rowIndexOf<Row>; }

	public:
		/// A plain row's value; a family is read by member (get<family>(member)). A live row does not compile: its
		/// value moves without a new config, so it is read through io.param, or liveValue in check and the change
		/// version.
		template <const auto& Row>
			requires(listed<Row> && !family<Row>)
		[[nodiscard]] typename std::remove_cvref_t<decltype(Row)>::Reads get() const {
			static_assert(!setting::isLive(Row.row()),
			              "a live row is read through io.param, or liveValue in check and the change version");
			return read<Row>();
		}
		/// A family's member, from 0 (its key's number less 1): the file's value, or the family's default when absent,
		/// active or not. A member at or above the family's maximum is a type bug: std::out_of_range. A live family
		/// does not compile, as get of a live row.
		template <const auto& Family>
			requires(listed<Family> && family<Family>)
		[[nodiscard]] typename std::remove_cvref_t<decltype(Family)>::Value get(std::size_t member) const {
			static_assert(!setting::isLive(Family.row()),
			              "a live row is read through io.param, or liveValue in check and the change version");
			return readMember<Family>(member);
		}
		/// A family's active members: its count row's value. Members above it are kept, inactive. A family whose count
		/// row is live does not compile: liveCount reads it.
		template <const auto& Family>
			requires(listed<Family> && family<Family>)
		[[nodiscard]] std::uint32_t count() const {
			static_assert(!setting::isLive(countRowOf<Family>),
			              "a family whose count row is live is counted by liveCount, or io.count in process");
			return parsedCount<Family>();
		}
		/// A live row's value as the node's live values hold it now (docs/writing-a-node-type.md#live-rows): in check
		/// at parse, what was parsed; in configure and the change version, the node's live values. A row that is not
		/// live does not compile: get reads it.
		template <const auto& Row>
			requires(listed<Row> && !family<Row>)
		[[nodiscard]] typename std::remove_cvref_t<decltype(Row)>::Value liveValue() const {
			static_assert(setting::isLive(Row.row()),
			              "liveValue reads a live row: a row that is not live is read by get");
			using Value = typename std::remove_cvref_t<decltype(Row)>::Value;
			if (settings_.live_ == nullptr) return read<Row>();
			return setting::liveValueOf<Value>(Row.row(), settings_.live_[liveFirst<Row>]);
		}
		/// A live family's member, from 0, as liveValue; at or above the maximum, std::out_of_range.
		template <const auto& Family>
			requires(listed<Family> && family<Family>)
		[[nodiscard]] typename std::remove_cvref_t<decltype(Family)>::Value liveValue(std::size_t member) const {
			static_assert(setting::isLive(Family.row()),
			              "liveValue reads a live row: a row that is not live is read by get");
			using Value = typename std::remove_cvref_t<decltype(Family)>::Value;
			if (settings_.live_ == nullptr || member >= Family.row().maxMembers) return readMember<Family>(member);
			return setting::liveValueOf<Value>(Family.row(), settings_.live_[liveFirst<Family> + member]);
		}
		/// The active members of a family whose count row is live, as liveValue reads that row.
		template <const auto& Family>
			requires(listed<Family> && family<Family>)
		[[nodiscard]] std::uint32_t liveCount() const {
			static_assert(setting::isLive(countRowOf<Family>),
			              "liveCount counts a family whose count row is live: count counts the others");
			if (settings_.live_ == nullptr) return parsedCount<Family>();
			return static_cast<std::uint32_t>(settings_.live_[liveAt(countRowOf<Family>)]);
		}

	private:
		template <const auto& Row> typename std::remove_cvref_t<decltype(Row)>::Reads read() const {
			using Read = std::remove_cvref_t<decltype(Row)>;
			using Value = typename Read::Value;
			constexpr bool choice = setting::isChoice(Row.row());
			using Slot = setting::SlotOf<Value, choice>;
			const auto* slot = std::any_cast<Slot>(&settings_.slots_[slotOf<Row>()]);
			const auto value = [](const Slot& held) -> Value {
				if constexpr (choice) return setting::choiceValue<Value>(Row.row(), held);
				else return held;
			};
			if constexpr (std::same_as<typename Read::Reads, Value>) {
				return value(*slot); // the walk filled it: a required row was given, a defaulted one has its default
			} else {
				return slot != nullptr ? typename Read::Reads(value(*slot)) : std::nullopt;
			}
		}
		template <const auto& Family>
		typename std::remove_cvref_t<decltype(Family)>::Value readMember(std::size_t member) const {
			using Value = typename std::remove_cvref_t<decltype(Family)>::Value;
			constexpr bool choice = setting::isChoice(Family.row());
			using Slot = setting::SlotOf<Value, choice>;
			if (member >= Family.row().maxMembers)
				throw std::out_of_range(std::string(Family.row().key) + ": no member " + std::to_string(member + 1));
			const auto value = [](const Slot& held) -> Value {
				if constexpr (choice) return setting::choiceValue<Value>(Family.row(), held);
				else return held;
			};
			const auto& given = std::any_cast<const FamilyValues<Slot>&>(settings_.slots_[slotOf<Family>()]);
			for (const auto& [number, held] : given)
				if (number == member + 1) return value(held);
			return value(std::get<Slot>(*Family.row().byDefault));
		}
		template <const auto& Family> std::uint32_t parsedCount() const {
			constexpr std::size_t slot = static_cast<std::size_t>(
				std::ranges::find(rows_, Family.row().indexedBy, &SettingRow::key) - rows_.begin());
			return std::any_cast<std::uint32_t>(settings_.slots_[slot]);
		}
		/// One live row's members' slot bits as parsed, appended to `out`.
		template <const auto& Row> void appendLive(std::vector<std::uint64_t>& out) const {
			if constexpr (setting::isLive(Row.row())) {
				using Value = typename std::remove_cvref_t<decltype(Row)>::Value;
				constexpr bool choice = setting::isChoice(Row.row());
				const auto bits = [](const Value& value) {
					if constexpr (choice) {
						const auto& names = std::get<ChoiceRule>(Row.row().rule);
						const auto underlying = static_cast<std::int64_t>(std::to_underlying(value));
						for (std::uint32_t at = 0; at < names.count; ++at)
							if (names.values[at] == underlying) return static_cast<std::uint64_t>(at);
						return std::uint64_t{0};
					} else {
						return setting::liveBits(value);
					}
				};
				if constexpr (family<Row>) {
					for (std::size_t member = 0; member < Row.row().maxMembers; ++member)
						out.push_back(bits(readMember<Row>(member)));
				} else {
					out.push_back(bits(read<Row>()));
				}
			}
		}
		/// Every live member's slot bits as parsed, in order: what seeds a node's live values
		/// (NodeConfig::liveValues).
		std::vector<std::uint64_t> parsedLive() const {
			std::vector<std::uint64_t> out;
			out.reserve(liveMemberCount);
			(appendLive<Rows>(out), ...);
			return out;
		}
		/// Each family's active members as parsed, by row (0 for a row that is not a family, or a family whose count
		/// row is live): what io.count gives a code made with these settings.
		std::array<std::uint32_t, rowCount> familyCounts() const {
			std::array<std::uint32_t, rowCount> counts{};
			[[maybe_unused]] const auto one = [&]<const auto & Row>() {
				if constexpr (family<Row>) {
					if constexpr (!setting::isLive(countRowOf<Row>)) counts[rowIndexOf<Row>] = parsedCount<Row>();
				}
			};
			(one.template operator()<Rows>(), ...);
			return counts;
		}
		explicit Checked(CheckedSettings settings) noexcept : settings_(std::move(settings)) {}
		friend SettingsTable;
		template <const auto& Spec, class Type> friend class spec_detail::SpecNodeType;
		template <const auto& Spec, class Type> friend class spec_detail::SpecConfig;
		template <const auto& Spec, class Type> friend class spec_detail::SpecInstance;
		template <const auto& Spec> friend class ear::CreateOf;
		template <const auto& Spec> friend class ear::ChangeOf;
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
