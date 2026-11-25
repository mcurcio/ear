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
/// One whole-number rule. A good value is a JSON integer (a float, a bool and a string are not) in [low, high]; an integer
/// above INT64_MAX is above every range. Otherwise "<key>: must be a whole number[ of <unit>], <low>-<high>".
struct WholeNumberRule {
	std::uint32_t low;
	std::uint32_t high;
	std::string_view unit;
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
};
/// One boolean rule. A good value is JSON true or false (not a string, a number or null); in ear.conf it is the bare word
/// true or false. Otherwise "<key>: must be true or false".
struct BooleanRule {};
/// Any one rule, as a row holds it.
using SettingRule = std::variant<WholeNumberRule, TextRule, RateRule, BooleanRule>;
/// One row as the walk reads it. A key given with any value, null included, is given. Anyone can fill one in, but a table
/// lists only typed rows, and a typed row gives its SettingRow to read, never to change.
struct SettingRow {
	std::string_view key;
	bool required; // absent: "<key>: missing"
	SettingRule rule;
	std::optional<std::uint32_t> byDefault = {}; // what the row reads when absent
	std::string_view onlyWithout = {};           // Row::onlyWithout's
	bool inPlace = false;                        // Row::inPlace's: a change to it may apply in place
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
/// A rate, read as SampleRate: written with its unit in ear.conf, a whole number of hertz on the control port. Low above
/// high does not compile.
consteval Rule<SampleRate> rate(SampleRate low, SampleRate high);
/// JSON true or false, read as bool: the bare word true or false in ear.conf.
consteval Rule<bool> boolean();
/// Parse: std::optional<T>(std::string_view), T not a view. T is what the row reads. An empty mustBe does not compile.
template <auto Parse>
	requires TextParser<Parse>
consteval Rule<ParsedBy<Parse>> text(std::string_view mustBe);

/// A checked rule whose value reads as T. Only number, rate, boolean and text make one, so T is always what the rule's
/// reader gives.
template <class T> class Rule {
public:
	using Value = T;
	constexpr const SettingRule& held() const noexcept { return rule_; }

private:
	consteval explicit Rule(SettingRule rule) : rule_(rule) {}
	friend consteval Rule<std::uint32_t> number(std::uint32_t low, std::uint32_t high, std::string_view unit);
	friend consteval Rule<SampleRate> rate(SampleRate low, SampleRate high);
	friend consteval Rule<bool> boolean();
	template <auto Parse>
		requires TextParser<Parse>
	friend consteval Rule<ParsedBy<Parse>> text(std::string_view mustBe);
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
	/// A change to this key may apply in place, through the type's change version of configure, while the node keeps
	/// running (docs/writing-a-node-type.md#settings-one-table-of-typed-rows). Unmarked, a row is restart-only: a change to it restarts the node.
	consteval Row inPlace() const {
		Row marked = *this;
		marked.row_.inPlace = true;
		return marked;
	}

private:
	// A throw in a consteval function is what stops the compile; the text is what the compiler shows.
	consteval Row(std::string_view key, bool required, SettingRule rule,
	              std::optional<std::uint32_t> byDefault = std::nullopt)
		: row_{key, required, rule, byDefault} {
		if (key.empty()) throw "a setting's key is empty";
		// The framework's key on every node (docs/architecture.md#the-node-lifecycle): never a type's, nor an indexed
		// family's base.
		if (key == "disabled" || key.starts_with("disabled."))
			throw "disabled is the framework's key: no type may declare it";
	}
	template <class V> friend consteval Row<V, true> required(std::string_view key, Rule<V> rule);
	template <class V> friend consteval Row<V, false> optional(std::string_view key, Rule<V> rule);
	friend consteval Row<std::uint32_t, true> defaulted(std::string_view key, Rule<std::uint32_t> rule,
	                                                    std::uint32_t byDefault);
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
consteval Rule<SampleRate> rate(SampleRate low, SampleRate high) {
	if (low > high) throw "a rate's low is above its high";
	return Rule<SampleRate>(RateRule{low, high});
}
consteval Rule<bool> boolean() { return Rule<bool>(BooleanRule{}); }
template <auto Parse>
	requires TextParser<Parse>
consteval Rule<ParsedBy<Parse>> text(std::string_view mustBe) {
	if (mustBe.empty()) throw "a text setting needs what it must be";
	return Rule<ParsedBy<Parse>>(TextRule{mustBe, &readParsed<Parse>});
}
template <class V> consteval Row<V, true> required(std::string_view key, Rule<V> rule) {
	return Row<V, true>(key, true, rule.held());
}
template <class V> consteval Row<V, false> optional(std::string_view key, Rule<V> rule) {
	return Row<V, false>(key, false, rule.held());
}
/// Numbers only; a default outside the range does not compile.
consteval Row<std::uint32_t, true> defaulted(std::string_view key, Rule<std::uint32_t> rule, std::uint32_t byDefault) {
	const WholeNumberRule range = std::get<WholeNumberRule>(rule.held());
	if (byDefault < range.low || byDefault > range.high) throw "a whole number's byDefault is outside its range";
	return Row<std::uint32_t, true>(key, false, rule.held(), byDefault);
}
constexpr SettingDeclaration declarationOf(const SettingRow& row) {
	if (std::holds_alternative<TextRule>(row.rule)) return {row.key, SettingKind::text};
	if (std::holds_alternative<RateRule>(row.rule)) return {row.key, SettingKind::rate};
	if (std::holds_alternative<BooleanRule>(row.rule)) return {row.key, SettingKind::boolean};
	return {row.key, SettingKind::wholeNumber};
}
} // namespace setting

template <const auto&... Rows>
	requires(setting::isRow<std::remove_cvref_t<decltype(Rows)>> && ...)
class SettingsTable;

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
	std::vector<std::any> slots_;
};

/// A table without its rows' types: all the walk needs, so the walk is not a template. Its constructor is consteval, so
/// run-time rows cannot reach the walk.
class AnySettingsTable {
public:
	constexpr std::string_view name() const noexcept { return name_; }
	/// NodeType::settingKinds(): every key, each once, in row order; the same span every call.
	constexpr std::span<const SettingDeclaration> kinds() const noexcept { return kinds_; }
	/// Whether the row of `key` is marked inPlace (Row::inPlace); false for a key the table lacks.
	constexpr bool changesInPlace(std::string_view key) const noexcept {
		for (const SettingRow* row : rows_)
			if (row->key == key) return row->inPlace;
		return false;
	}

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
	std::expected<CheckedSettings, std::string> walk(const NodeSettings& settings) const;

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
		CheckedSettings settings_;
	};
	/// The walk with no node type around it, for tests; production goes through NodeType::parse.
	std::expected<Checked, std::string> check(const NodeSettings& settings) const {
		auto walked = walk(settings);
		if (!walked) return std::unexpected(std::move(walked.error()));
		return Checked(std::move(*walked));
	}

private:
	static constexpr std::array<const SettingRow*, sizeof...(Rows)> rows_{&Rows.row()...};
	static constexpr std::array<SettingDeclaration, sizeof...(Rows)> kinds_{setting::declarationOf(Rows.row())...};
};
} // namespace ear
