// What a node type declares about what it reports (docs/writing-a-node-type.md#status-and-events): its status fields
// (facts with a current value, StatusTable), its events (things that happened, EventTable) and its realtime log codes
// (LogCodes), each a compile-time constant in the type's named namespace, listed in its TypeSpec. Report<spec>
// (report.hpp) writes them.
//
// Pure: nothing but the standard library and ear_core.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>
#include "ear/core/log.hpp"
#include "ear/core/status_declarations.hpp"
#include "ear/core/loop.hpp"
#include "ear/core/settings_table.hpp" // linkage::named
namespace ear {
namespace spec_detail {
/// A status field's name, an event's or a log code's code, as a template argument.
template <std::size_t Size> struct Name {
	char text[Size]{};
	consteval Name(const char (&name)[Size]) { // NOLINT: implicit on purpose, from a string literal
		std::copy_n(name, Size, text);
	}
	constexpr std::string_view view() const noexcept { return {text, Size - 1}; }
};
/// camelCase: a lower-case letter, then letters and digits.
consteval bool validFieldName(std::string_view name) {
	return !name.empty() && name.front() >= 'a' && name.front() <= 'z' && std::ranges::all_of(name, [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
	});
}
/// kebab-case: [a-z0-9-], starting with a letter. No `.`, so never one of the framework's `ear.` codes.
consteval bool validCode(std::string_view code) {
	return !code.empty() && code.front() >= 'a' && code.front() <= 'z' && std::ranges::all_of(code, [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
	});
}
} // namespace spec_detail

namespace writer {
inline constexpr Writer rt{Writer::Kind::rt};
/// A loop the spec lists in `.loops` (TypeSpec checks it).
template <class Handle> consteval Writer loop(Handle) {
	static_assert(isLoopHandle<Handle>, "writer::loop takes a loop handle");
	return Writer{Writer::Kind::loop, Handle::name};
}
inline constexpr Writer companion{Writer::Kind::companion};
} // namespace writer

/// A choice field's value, made at compile time by the field (`connection("connected")`), so a misspelt value does not
/// compile.
struct Choice {
	std::size_t index;
	friend constexpr bool operator==(Choice, Choice) = default;
};

namespace status {
/// `.initially(status::unknown)`, `report.set<field>(status::unknown)`: the field's value is null.
inline constexpr struct Unknown {
} unknown;

/// What a field of kind K takes from its writer: a flag bool, a number std::int64_t, a decimal double, a choice Choice
/// (or its text, checked at run time), a text std::string_view.
template <ValueKind K> struct ValueOf;
template <> struct ValueOf<ValueKind::flag> {
	using Type = bool;
};
template <> struct ValueOf<ValueKind::wholeNumber> {
	using Type = std::int64_t;
};
template <> struct ValueOf<ValueKind::decimal> {
	using Type = double;
};
template <> struct ValueOf<ValueKind::choice> {
	using Type = Choice;
};
template <> struct ValueOf<ValueKind::text> {
	using Type = std::string_view;
};

template <ValueKind K> class Field;
template <spec_detail::Name N> consteval Field<ValueKind::flag> flag(writer::Writer w);
template <spec_detail::Name N> consteval Field<ValueKind::wholeNumber>
number(writer::Writer w, std::int64_t low, std::int64_t high, std::string_view unit = {});
template <spec_detail::Name N> consteval Field<ValueKind::decimal>
decimal(writer::Writer w, double low, double high, std::string_view unit = {}, std::uint8_t decimals = 2);
template <spec_detail::Name N, std::size_t Count>
consteval Field<ValueKind::choice> choice(writer::Writer w, const std::string_view (&values)[Count]);
template <spec_detail::Name N> consteval Field<ValueKind::text> text(writer::Writer w, std::uint32_t maxBytes = 256);

/// One status field, made only by status::flag, number, decimal, choice and text.
template <ValueKind K> class Field {
public:
	static constexpr ValueKind kind = K;
	using Value = typename ValueOf<K>::Type;
	constexpr const StatusDeclaration& declaration() const noexcept { return declared_; }
	consteval Field display(Display display) const {
		Field marked = *this;
		marked.declared_.display = display;
		return marked;
	}
	consteval Field help(std::string_view line) const {
		if (line.empty()) throw "a status field's help is a line, never empty";
		Field marked = *this;
		marked.declared_.help = line;
		return marked;
	}
	consteval Field flapsAbove(std::uint32_t transitions, std::uint32_t windowMs) const {
		if (transitions == 0 || windowMs == 0) throw "a field's flapsAbove is above 0 changes in more than 0 ms";
		Field marked = *this;
		marked.declared_.flapsAbove = {transitions, windowMs};
		return marked;
	}
	consteval Field initially(Unknown) const {
		Field marked = *this;
		marked.declared_.initialUnknown = true;
		return marked;
	}
	/// A value of the field's kind, within its range, among its values or within its bytes.
	template <class Given> consteval Field initially(Given value) const {
		Field marked = *this;
		marked.declared_.initialUnknown = false;
		if constexpr (K == ValueKind::flag) {
			static_assert(std::same_as<Given, bool>, "a flag field's initial value is true or false");
			marked.declared_.initialFlag = value;
		} else if constexpr (K == ValueKind::wholeNumber) {
			static_assert(std::is_integral_v<Given> && !std::same_as<Given, bool>,
			              "a whole-number field's initial value is a whole number");
			const auto number = static_cast<std::int64_t>(value);
			if (number < declared_.low || number > declared_.high)
				throw "a status field's initial value is outside its range";
			marked.declared_.initialNumber = number;
		} else if constexpr (K == ValueKind::decimal) {
			static_assert(std::is_arithmetic_v<Given> && !std::same_as<Given, bool>,
			              "a decimal field's initial value is a number");
			const auto number = static_cast<double>(value);
			if (number < declared_.lowDecimal || number > declared_.highDecimal)
				throw "a status field's initial value is outside its range";
			marked.declared_.initialDecimal = number;
		} else if constexpr (K == ValueKind::choice) {
			static_assert(std::convertible_to<Given, std::string_view>,
			              "a choice field's initial value is one of its values");
			marked.declared_.initialChoice = (*this)(std::string_view(value)).index;
		} else {
			static_assert(std::convertible_to<Given, std::string_view>, "a text field's initial value is a text");
			const std::string_view text(value);
			if (text.size() > declared_.maxBytes) throw "a status field's initial value is outside its range";
			marked.declared_.initialText = text;
		}
		return marked;
	}
	/// A choice field's value by its text: one of its values, or no compile.
	consteval Choice operator()(std::string_view value) const
		requires(K == ValueKind::choice)
	{
		for (std::size_t at = 0; at < declared_.values.size(); ++at)
			if (declared_.values[at] == value) return Choice{at};
		throw "not one of the choice field's values";
	}

private:
	consteval explicit Field(StatusDeclaration declared) : declared_(declared) {
		if (!spec_detail::validFieldName(declared.name)) throw "a status field's name is camelCase: [a-z][A-Za-z0-9]*";
		if (declared.writer.kind == writer::Writer::Kind::rt && K == ValueKind::text)
			throw "a text field is never written from the realtime thread: writer::rt takes a flag, number, decimal "
				  "or choice";
	}
	template <spec_detail::Name N> friend consteval Field<ValueKind::flag> flag(writer::Writer w);
	template <spec_detail::Name N> friend consteval Field<ValueKind::wholeNumber>
	number(writer::Writer w, std::int64_t low, std::int64_t high, std::string_view unit);
	template <spec_detail::Name N> friend consteval Field<ValueKind::decimal>
	decimal(writer::Writer w, double low, double high, std::string_view unit, std::uint8_t decimals);
	template <spec_detail::Name N, std::size_t Count>
	friend consteval Field<ValueKind::choice> choice(writer::Writer w, const std::string_view (&values)[Count]);
	template <spec_detail::Name N>
	friend consteval Field<ValueKind::text> text(writer::Writer w, std::uint32_t maxBytes);
	StatusDeclaration declared_;
};
template <class T> inline constexpr bool isField = false;
template <ValueKind K> inline constexpr bool isField<Field<K>> = true;

/// A flag, false until written.
template <spec_detail::Name N> consteval Field<ValueKind::flag> flag(writer::Writer w) {
	return Field<ValueKind::flag>(StatusDeclaration{.name = N.view(), .kind = ValueKind::flag, .writer = w});
}
/// A whole number in [low, high], 0 (or the end of the range nearer it) until written.
template <spec_detail::Name N> consteval Field<ValueKind::wholeNumber>
number(writer::Writer w, std::int64_t low, std::int64_t high, std::string_view unit) {
	if (low > high) throw "a status field's low is above its high";
	return Field<ValueKind::wholeNumber>(StatusDeclaration{.name = N.view(),
	                                                       .kind = ValueKind::wholeNumber,
	                                                       .writer = w,
	                                                       .low = low,
	                                                       .high = high,
	                                                       .unit = unit,
	                                                       .initialNumber = std::clamp<std::int64_t>(0, low, high)});
}
/// A decimal in [low, high], shown with `decimals` places; 0 (or the end of the range nearer it) until written.
template <spec_detail::Name N> consteval Field<ValueKind::decimal>
decimal(writer::Writer w, double low, double high, std::string_view unit, std::uint8_t decimals) {
	if (!(low <= high)) throw "a status field's low is above its high";
	return Field<ValueKind::decimal>(StatusDeclaration{.name = N.view(),
	                                                   .kind = ValueKind::decimal,
	                                                   .writer = w,
	                                                   .lowDecimal = low,
	                                                   .highDecimal = high,
	                                                   .decimals = decimals,
	                                                   .unit = unit,
	                                                   .initialDecimal = std::clamp(0.0, low, high)});
}
/// One of `values` (named once; unique, never empty), unknown until written.
template <spec_detail::Name N, std::size_t Count>
consteval Field<ValueKind::choice> choice(writer::Writer w, const std::string_view (&values)[Count]) {
	for (std::size_t at = 0; at < Count; ++at) {
		if (values[at].empty()) throw "a choice field's values are never empty";
		for (std::size_t earlier = 0; earlier < at; ++earlier)
			if (values[earlier] == values[at]) throw "a choice field names a value twice";
	}
	return Field<ValueKind::choice>(StatusDeclaration{.name = N.view(),
	                                                  .kind = ValueKind::choice,
	                                                  .writer = w,
	                                                  .values = std::span<const std::string_view>(values),
	                                                  .initialUnknown = true});
}
/// A text of at most `maxBytes` bytes (cut, and marked, beyond), "" until written. Never writer::rt.
template <spec_detail::Name N> consteval Field<ValueKind::text> text(writer::Writer w, std::uint32_t maxBytes) {
	if (maxBytes == 0) throw "a text field holds at least one byte";
	return Field<ValueKind::text>(
		StatusDeclaration{.name = N.view(), .kind = ValueKind::text, .writer = w, .maxBytes = maxBytes});
}
} // namespace status

namespace arg {
/// What an argument of kind K takes: a flag bool, a number std::int64_t, a decimal double, a choice or a text its
/// std::string_view (a choice's checked against its values at run time).
template <ValueKind K> struct ValueOf {
	using Type =
		std::conditional_t<K == ValueKind::flag, bool,
	                       std::conditional_t<K == ValueKind::wholeNumber, std::int64_t,
	                                          std::conditional_t<K == ValueKind::decimal, double, std::string_view>>>;
};
/// One argument, made only by arg::number, decimal, flag, choice and text.
template <ValueKind K> struct Arg {
	static constexpr ValueKind kind = K;
	using Value = typename ValueOf<K>::Type;
	ArgDeclaration declared;
};
template <class T> inline constexpr bool isArg = false;
template <ValueKind K> inline constexpr bool isArg<Arg<K>> = true;
template <spec_detail::Name N> consteval Arg<ValueKind::wholeNumber> number(std::int64_t low, std::int64_t high) {
	if (!spec_detail::validFieldName(N.view())) throw "an argument's name is camelCase: [a-z][A-Za-z0-9]*";
	if (low > high) throw "an argument's low is above its high";
	return {{.name = N.view(), .kind = ValueKind::wholeNumber, .low = low, .high = high}};
}
template <spec_detail::Name N> consteval Arg<ValueKind::decimal> decimal(std::uint8_t decimals = 2) {
	if (!spec_detail::validFieldName(N.view())) throw "an argument's name is camelCase: [a-z][A-Za-z0-9]*";
	return {{.name = N.view(), .kind = ValueKind::decimal, .decimals = decimals}};
}
template <spec_detail::Name N> consteval Arg<ValueKind::flag> flag() {
	if (!spec_detail::validFieldName(N.view())) throw "an argument's name is camelCase: [a-z][A-Za-z0-9]*";
	return {{.name = N.view(), .kind = ValueKind::flag}};
}
template <spec_detail::Name N, std::size_t Count>
consteval Arg<ValueKind::choice> choice(const std::string_view (&values)[Count]) {
	if (!spec_detail::validFieldName(N.view())) throw "an argument's name is camelCase: [a-z][A-Za-z0-9]*";
	return {{.name = N.view(), .kind = ValueKind::choice, .values = std::span<const std::string_view>(values)}};
}
/// Never from `process`.
template <spec_detail::Name N> consteval Arg<ValueKind::text> text(std::uint32_t maxBytes = 256) {
	if (!spec_detail::validFieldName(N.view())) throw "an argument's name is camelCase: [a-z][A-Za-z0-9]*";
	if (maxBytes == 0) throw "a text argument holds at least one byte";
	return {{.name = N.view(), .kind = ValueKind::text, .maxBytes = maxBytes}};
}
} // namespace arg

namespace spec_detail {
/// A sentence names each argument once as {name}, and every place names an argument.
template <std::size_t Count>
consteval void checkSentence(std::string_view sentence, const std::array<ArgDeclaration, Count>& args) {
	if (sentence.empty()) throw "an event's sentence is never empty";
	std::array<std::size_t, Count> placed{};
	for (std::size_t at = 0; at < sentence.size(); ++at) {
		if (sentence[at] == '}') throw "a sentence's '}' closes no place";
		if (sentence[at] != '{') continue;
		const std::size_t end = sentence.find('}', at);
		if (end == std::string_view::npos) throw "a sentence's '{' opens a place it never closes";
		const std::string_view place = sentence.substr(at + 1, end - at - 1);
		bool found = false;
		for (std::size_t one = 0; one < Count; ++one)
			if (args[one].name == place) {
				++placed[one];
				found = true;
			}
		if (!found) throw "a sentence's place names no argument";
		at = end;
	}
	for (std::size_t one = 0; one < Count; ++one) {
		if (placed[one] == 0) throw "an argument has no place in the sentence";
		if (placed[one] > 1) throw "an argument has two places in the sentence";
		for (std::size_t earlier = 0; earlier < one; ++earlier)
			if (args[earlier].name == args[one].name) throw "an event names an argument twice";
	}
}
} // namespace spec_detail

namespace event {
/// One event, made only by event::of.
template <class... Args> class Event {
public:
	static constexpr std::size_t arity = sizeof...(Args);
	/// What raise takes, in order.
	using Values = std::tuple<typename Args::Value...>;
	constexpr EventDeclaration declaration() const noexcept { return {code_, severity_, sentence_, args_}; }
	constexpr std::string_view code() const noexcept { return code_; }

private:
	consteval Event(std::string_view code, EventSeverity severity, std::string_view sentence, Args... args)
		: code_(code), severity_(severity), sentence_(sentence), args_{args.declared...} {
		if (!spec_detail::validCode(code)) throw "an event's code is kebab-case: [a-z][a-z0-9-]*";
		spec_detail::checkSentence(sentence, args_);
	}
	template <spec_detail::Name Code, class... Given>
	friend consteval Event<Given...> of(EventSeverity severity, std::string_view sentence, Given... args);
	std::string_view code_;
	EventSeverity severity_;
	std::string_view sentence_;
	std::array<ArgDeclaration, sizeof...(Args)> args_;
};
template <class T> inline constexpr bool isEvent = false;
template <class... Args> inline constexpr bool isEvent<Event<Args...>> = true;
/// `sentence` names each argument once as {name}; a place that names no argument, or an argument with no place, does
/// not compile. It is the record's msg and the editor's line.
template <spec_detail::Name Code, class... Args>
consteval Event<Args...> of(EventSeverity severity, std::string_view sentence, Args... args) {
	static_assert((arg::isArg<Args> && ...), "an event's arguments are arg:: makers'");
	return Event<Args...>(Code.view(), severity, sentence, args...);
}
} // namespace event

/// One realtime log code as the framework reads it (slice 5.3's Io::log).
struct LogCodeDeclaration {
	std::string_view code;
	LogLevel level;
	std::string_view sentence;
	std::span<const ArgDeclaration> args;
};

namespace log {
/// One log code, made only by log::code: numbers, decimals and flags, at most four.
template <class... Args> class LogCode {
public:
	static constexpr std::size_t arity = sizeof...(Args);
	constexpr LogCodeDeclaration declaration() const noexcept { return {code_, level_, sentence_, args_}; }
	constexpr std::string_view code() const noexcept { return code_; }

private:
	consteval LogCode(std::string_view code, LogLevel level, std::string_view sentence, Args... args)
		: code_(code), level_(level), sentence_(sentence), args_{args.declared...} {
		if (!spec_detail::validCode(code)) throw "a log code is kebab-case: [a-z][a-z0-9-]*";
		spec_detail::checkSentence(sentence, args_);
	}
	template <spec_detail::Name Code, class... Given>
	friend consteval LogCode<Given...> code(LogLevel level, std::string_view sentence, Given... args);
	std::string_view code_;
	LogLevel level_;
	std::string_view sentence_;
	std::array<ArgDeclaration, sizeof...(Args)> args_;
};
template <class T> inline constexpr bool isLogCode = false;
template <class... Args> inline constexpr bool isLogCode<LogCode<Args...>> = true;
template <spec_detail::Name Code, class... Args>
consteval LogCode<Args...> code(LogLevel level, std::string_view sentence, Args... args) {
	static_assert((arg::isArg<Args> && ...), "a log code's arguments are arg:: makers'");
	static_assert(
		((Args::kind == ValueKind::wholeNumber || Args::kind == ValueKind::decimal || Args::kind == ValueKind::flag) &&
	     ...),
		"a log code's arguments are numbers, decimals and flags: the realtime thread writes no text");
	static_assert(sizeof...(Args) <= 4, "a log code has at most four arguments");
	return LogCode<Args...>(Code.view(), level, sentence, args...);
}
} // namespace log

namespace spec_detail {
template <std::size_t Count> consteval bool uniqueViews(const std::array<std::string_view, Count>& names) {
	for (std::size_t at = 0; at < Count; ++at)
		for (std::size_t earlier = 0; earlier < at; ++earlier)
			if (names[at] == names[earlier]) return false;
	return true;
}
template <const auto& Which, const auto&... Among> consteval std::size_t referenceIndex() {
	constexpr std::array<const void*, sizeof...(Among)> addresses{static_cast<const void*>(&Among)...};
	return static_cast<std::size_t>(std::ranges::find(addresses, static_cast<const void*>(&Which)) - addresses.begin());
}
} // namespace spec_detail

/// A spec's status fields (TypeSpec's `.status`), in declaration order: unique names, each in a named namespace, at
/// most 64 written from the realtime thread.
template <const auto&... Fields>
	requires(status::isField<std::remove_cvref_t<decltype(Fields)>> && ...)
struct StatusTable {
	static_assert((linkage::named<Fields> && ...),
	              "a type's status fields are declared in a named namespace, never an unnamed one");
	static constexpr std::size_t count = sizeof...(Fields);
	static_assert(spec_detail::uniqueViews(std::array<std::string_view, count>{Fields.declaration().name...}),
	              "a type's status fields have unique names");
	static_assert(((Fields.declaration().writer.kind == writer::Writer::Kind::rt ? 1 : 0) + ... + 0) <= 64,
	              "a type has at most 64 status fields written from the realtime thread");
	/// Whether `Field` is one of these fields.
	template <const auto& Field> static constexpr bool has =
		((static_cast<const void*>(&Field) == static_cast<const void*>(&Fields)) || ...);
	template <const auto& Field> static constexpr std::size_t indexOf = spec_detail::referenceIndex<Field, Fields...>();
	static constexpr std::array<const StatusDeclaration*, count> declared{&Fields.declaration()...};
};
template <class T> inline constexpr bool isStatusTable = false;
template <const auto&... Fields> inline constexpr bool isStatusTable<StatusTable<Fields...>> = true;

/// A spec's events (TypeSpec's `.events`), in declaration order: unique codes, each in a named namespace.
template <const auto&... Events>
	requires(event::isEvent<std::remove_cvref_t<decltype(Events)>> && ...)
struct EventTable {
	static_assert((linkage::named<Events> && ...),
	              "a type's events are declared in a named namespace, never an unnamed one");
	static constexpr std::size_t count = sizeof...(Events);
	static_assert(spec_detail::uniqueViews(std::array<std::string_view, count>{Events.code()...}),
	              "a type's events have unique codes");
	template <const auto& Event> static constexpr bool has =
		((static_cast<const void*>(&Event) == static_cast<const void*>(&Events)) || ...);
	template <const auto& Event> static constexpr std::size_t indexOf = spec_detail::referenceIndex<Event, Events...>();
	static constexpr std::array<EventDeclaration, count> declared{Events.declaration()...};
};
template <class T> inline constexpr bool isEventTable = false;
template <const auto&... Events> inline constexpr bool isEventTable<EventTable<Events...>> = true;

/// A spec's realtime log codes (TypeSpec's `.logCodes`): unique codes, each in a named namespace.
template <const auto&... Codes>
	requires(log::isLogCode<std::remove_cvref_t<decltype(Codes)>> && ...)
struct LogCodes {
	static_assert((linkage::named<Codes> && ...),
	              "a type's log codes are declared in a named namespace, never an unnamed one");
	static constexpr std::size_t count = sizeof...(Codes);
	static_assert(spec_detail::uniqueViews(std::array<std::string_view, count>{Codes.code()...}),
	              "a type's log codes are unique");
	template <const auto& Code> static constexpr bool has =
		((static_cast<const void*>(&Code) == static_cast<const void*>(&Codes)) || ...);
	static constexpr std::array<LogCodeDeclaration, count> declared{Codes.declaration()...};
};
template <class T> inline constexpr bool isLogCodes = false;
template <const auto&... Codes> inline constexpr bool isLogCodes<LogCodes<Codes...>> = true;
} // namespace ear
