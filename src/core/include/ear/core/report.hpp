// Report<spec>: a node's status writes and events off the realtime thread (docs/writing-a-node-type.md#report), by the
// spec's own field and event handles, carried in the same per-configuration channel as its handle's `lost`.
#pragma once
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include "ear/core/node_handle.hpp"
#include "ear/core/node_status.hpp"
#include "ear/core/type_spec.hpp"
namespace ear {
template <const auto& Spec> class NodeOf;
namespace testing {
template <const auto& Spec, class Type> class TypeHarness;
struct ReportSeam;
} // namespace testing

/// The typed control-plane face of one configuration, over its NodeHandle: status writes and events by the spec's
/// handles, from the loop each field names (docs/writing-a-node-type.md#report). Copyable; never blocks but on the
/// queue's one short mutex. A write lands on the configuration that made it until that configuration closed, so a
/// goodbye written during its teardown is heard; once it closed, writes are dropped. A text is made safe here, on the
/// writer's thread (safeText). A value that breaks its declaration (a choice text not among its values, a number
/// outside its range, a decimal that is not finite) is the type's bug: the node fails `lost` with the rule's text,
/// through the same channel, and nothing is written.
template <const auto& Spec> class Report {
	static_assert(linkage::named<Spec>, "a spec is declared in a named namespace, never an unnamed one");
	using Fields = StatusOf<Spec>;
	using Events = EventsOf<Spec>;

public:
	/// Field `Field`, a writer::loop (or writer::companion) field of the spec, now `value`: a flag bool, a number
	/// std::int64_t, a decimal double, a choice its Choice (`field("value")`, checked at compile time), a text its text.
	template <const auto& Field> void set(typename std::remove_cvref_t<decltype(Field)>::Value value) const {
		checkField<Field>();
		const StatusDeclaration& declared = Field.declaration();
		using Kind = std::remove_cvref_t<decltype(Field)>;
		if constexpr (Kind::kind == ValueKind::flag) write<Field>(value);
		else if constexpr (Kind::kind == ValueKind::wholeNumber) {
			if (value < declared.low || value > declared.high)
				return broken("status " + std::string(declared.name) + ": " + std::to_string(value) + " is outside " +
				              std::to_string(declared.low) + " to " + std::to_string(declared.high));
			write<Field>(static_cast<std::int64_t>(value));
		} else if constexpr (Kind::kind == ValueKind::decimal) {
			if (!std::isfinite(value) || value < declared.lowDecimal || value > declared.highDecimal)
				return broken("status " + std::string(declared.name) + ": " + valueText(value, declared.decimals) +
				              " is outside " + valueText(declared.lowDecimal, declared.decimals) + " to " +
				              valueText(declared.highDecimal, declared.decimals));
			write<Field>(value);
		} else if constexpr (Kind::kind == ValueKind::choice) write<Field>(std::string(declared.values[value.index]));
		else write<Field>(safeText(value, declared.maxBytes));
	}
	/// A choice field by its text, checked at run time: a text not among its values fails the node.
	template <const auto& Field>
		requires(std::remove_cvref_t<decltype(Field)>::kind == ValueKind::choice)
	void set(std::string_view text) const {
		checkField<Field>();
		const StatusDeclaration& declared = Field.declaration();
		for (const std::string_view value : declared.values)
			if (value == text) return write<Field>(std::string(value));
		broken("status " + std::string(declared.name) + ": \"" + safeText(text, 64) + "\" is not one of its values");
	}
	/// Field `Field` unknown (null).
	template <const auto& Field> void set(status::Unknown) const {
		checkField<Field>();
		write<Field>(std::monostate{});
	}
	/// Event `Event` of the spec, with one value per argument in its declared order: a number std::int64_t, a decimal
	/// double, a flag bool, a choice or a text a std::string_view (a choice's checked against its values here).
	template <const auto& Event, class... Values> void raise(Values&&... values) const {
		static_assert(Events::template has<Event>, "raise takes only its spec's events");
		using Declared = std::remove_cvref_t<decltype(Event)>;
		static_assert(sizeof...(Values) == Declared::arity, "raise takes one value per argument of the event");
		const EventDeclaration declared = Event.declaration();
		std::vector<StatusValue> args;
		args.reserve(sizeof...(Values));
		std::optional<std::string> rule;
		[&]<std::size_t... At>(std::index_sequence<At...>) {
			(argument<std::tuple_element_t<At, typename Declared::Values>>(declared, At, std::forward<Values>(values),
			                                                               args, rule),
			 ...);
		}(std::index_sequence_for<Values...>{});
		if (rule) return broken(std::move(*rule));
		handle_->reportEvent(Events::template indexOf<Event>, std::move(args));
	}
	/// The configuration's handle (lost, log).
	NodeHandle& handle() const noexcept { return *handle_; }

private:
	friend class NodeOf<Spec>;
	template <const auto&, class> friend class testing::TypeHarness;
	friend struct testing::ReportSeam;
	explicit Report(NodeHandle& handle) noexcept : handle_(&handle) {}
	template <const auto& Field> static constexpr void checkField() {
		static_assert(Fields::template has<Field>, "set takes only its spec's status fields");
		static_assert(Field.declaration().writer.kind != writer::Writer::Kind::rt,
		              "a writer::rt field is written by the node's code, through Io, never through Report");
	}
	template <const auto& Field> void write(StatusValue value) const {
		handle_->reportStatus(Fields::template indexOf<Field>, std::move(value));
	}
	void broken(std::string rule) const { handle_->lost(std::move(rule)); }
	template <class Expected, class Given> static void argument(const EventDeclaration& event, std::size_t at,
	                                                            Given&& given, std::vector<StatusValue>& args,
	                                                            std::optional<std::string>& rule) {
		static_assert(std::is_convertible_v<Given, Expected>, "an event's argument is of its declared kind");
		const ArgDeclaration& declared = event.args[at];
		const auto refuse = [&](std::string why) {
			if (!rule) rule = "event " + std::string(event.code) + ": " + std::string(declared.name) + " " + why;
		};
		const Expected value = std::forward<Given>(given);
		switch (declared.kind) {
		case ValueKind::flag:
		case ValueKind::wholeNumber:
		case ValueKind::decimal:
			if constexpr (std::same_as<Expected, std::int64_t>) {
				if (value < declared.low || value > declared.high)
					refuse(std::to_string(value) + " is outside " + std::to_string(declared.low) + " to " +
					       std::to_string(declared.high));
			}
			if constexpr (std::same_as<Expected, double>) {
				if (!std::isfinite(value)) refuse("is not a finite number");
			}
			if constexpr (!std::same_as<Expected, std::string_view>) args.emplace_back(value);
			return;
		case ValueKind::choice:
			if constexpr (std::same_as<Expected, std::string_view>) {
				for (const std::string_view one : declared.values)
					if (one == value) {
						args.emplace_back(std::string(one));
						return;
					}
				refuse("\"" + safeText(value, 64) + "\" is not one of its values");
				args.emplace_back(std::monostate{});
			}
			return;
		case ValueKind::text:
			if constexpr (std::same_as<Expected, std::string_view>)
				args.emplace_back(safeText(value, declared.maxBytes));
			return;
		}
	}
	NodeHandle* handle_;
};
} // namespace ear
