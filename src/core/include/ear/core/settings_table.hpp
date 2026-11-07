// One node type's settings, each key declared once, as typed rows in one table. The table checks the settings object in
// one fixed order and makes every refusal's text from the row; NodeType's name(), settingKinds() and parse() all answer
// from it and cannot be overridden, so the graph file's reader cannot disagree with parse. What a type overrides is
// NodeTypeOf's make, which gets typed values, never the raw object, and holds any rule about them.
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
namespace ear {
/// One whole-number rule. A good value is a JSON integer (a float, a bool and a string are not) in [low, high]; an integer
/// above INT64_MAX is above every range. Otherwise "<key>: must be a whole number[ of <unit>], <low>-<high>".
struct WholeNumberRule {
	std::uint32_t low;
	std::uint32_t high;
	std::string_view unit;
};
/// One text rule. A good value is a JSON string `read` accepts; `read` returns the parsed value, empty when refused.
/// Otherwise "<key>: must be <mustBe>".
struct TextRule {
	std::string_view mustBe;
	std::any (*read)(std::string_view text);
};
/// One row as the walk reads it. A key given with any value, null included, is given. Anyone can fill one in, but a table
/// lists only typed rows, and a typed row gives its SettingRow to read, never to change.
struct SettingRow {
	std::string_view key;
	bool required; // absent: "<key>: missing"
	std::variant<WholeNumberRule, TextRule> rule;
	std::optional<std::uint32_t> byDefault = {}; // what the row reads when absent
	std::string_view onlyWithout = {};           // Row::onlyWithout's
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
/// Parse: std::optional<T>(std::string_view), T not a view. T is what the row reads. An empty mustBe does not compile.
template <auto Parse>
	requires TextParser<Parse>
consteval Rule<ParsedBy<Parse>> text(std::string_view mustBe);

/// A checked rule whose value reads as T. Only number and text make one, so T is always what the rule's reader gives.
template <class T> class Rule {
public:
	using Value = T;
	constexpr const std::variant<WholeNumberRule, TextRule>& held() const noexcept { return rule_; }

private:
	consteval explicit Rule(std::variant<WholeNumberRule, TextRule> rule) : rule_(rule) {}
	friend consteval Rule<std::uint32_t> number(std::uint32_t low, std::uint32_t high, std::string_view unit);
	template <auto Parse>
		requires TextParser<Parse>
	friend consteval Rule<ParsedBy<Parse>> text(std::string_view mustBe);
	std::variant<WholeNumberRule, TextRule> rule_;
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

private:
	// A throw in a consteval function is what stops the compile; the text is what the compiler shows.
	consteval Row(std::string_view key, bool required, std::variant<WholeNumberRule, TextRule> rule,
	              std::optional<std::uint32_t> byDefault = std::nullopt)
		: row_{key, required, rule, byDefault} {
		if (key.empty()) throw "a setting's key is empty";
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
	return {row.key, std::holds_alternative<TextRule>(row.rule) ? SettingKind::text : SettingKind::wholeNumber};
}
} // namespace setting

template <const auto&... Rows>
	requires(setting::isRow<std::remove_cvref_t<decltype(Rows)>> && ...)
class SettingsTable;
template <const auto& Table> class NodeTypeOf;

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
		template <const auto& Table> friend class NodeTypeOf;
		template <const auto& Spec, class Behaviour> friend class spec_detail::SpecNodeType;
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

/// What a node type derives from. The base class names the table once; there is nowhere to name a second.
template <const auto& Table> class NodeTypeOf : public NodeType {
#ifdef EAR_NODE_TYPE_NAME // ear_node_type() (cmake/NodeType.cmake) gives a type's library its folder's name
	static_assert(Table.name() == EAR_NODE_TYPE_NAME, "a type's table is named for its folder");
#endif

protected:
	using Checked = typename std::remove_cvref_t<decltype(Table)>::Checked;
	NodeTypeOf() noexcept : NodeType(Table) {}

private:
	/// What the type makes of checked settings. It never sees the raw object. A rule about values lives here.
	virtual MadeConfig make(const NodeName& node, const Checked& settings) const = 0;
	MadeConfig makeConfig(const NodeName& node, CheckedSettings settings) const final {
		return make(node, Checked(std::move(settings)));
	}
};
} // namespace ear
