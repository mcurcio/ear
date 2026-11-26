// What a node type declares that a client may ask of it (docs/writing-a-node-type.md#actions): its actions, each a
// compile-time constant in the type's named namespace, listed in its TypeSpec's `.actions`. A trigger reaches the code,
// wait-free (Io::triggered); a call reaches one loop's state, with arguments checked by a SettingsTable, and its
// handler's answer is the reply. NodeHost carries both (NodeActions, node_host.hpp).
//
// Pure: nothing but the standard library and ear_core.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include "ear/core/loop.hpp"
#include "ear/core/node_type.hpp"
#include "ear/core/settings_table.hpp"
#include "ear/core/status_spec.hpp" // spec_detail::Name, validCode, uniqueViews, referenceIndex
namespace ear {
namespace spec_detail {
/// The framework's lifecycle requests (docs/control-port.md#methods), which no type action may be named.
consteval bool lifecycleName(std::string_view name) {
	return name == "start" || name == "stop" || name == "restart" || name == "disable" || name == "enable";
}
consteval void checkActionName(std::string_view name, std::string_view help) {
	if (!validCode(name)) throw "an action's name is kebab-case: [a-z][a-z0-9-]*";
	if (lifecycleName(name))
		throw "an action is never named start, stop, restart, disable or enable: those are the framework's";
	if (help.empty()) throw "an action's help is a line, never empty";
}
/// A call's checked arguments, made from what its table's walk made (SettingsTable::Checked has no other maker).
struct ArgsAccess {
	template <class Checked> static Checked make(CheckedSettings&& walked) { return Checked(std::move(walked)); }
};
} // namespace spec_detail
namespace action {
/// One trigger: into the node's code, with no arguments; made only by action::trigger.
template <spec_detail::Name N> class Trigger {
public:
	static constexpr bool isCall = false;
	constexpr ActionDeclaration declaration() const noexcept {
		return {.name = N.view(), .target = ActionDeclaration::Target::code, .help = help_};
	}

private:
	consteval explicit Trigger(std::string_view help) : help_(help) { spec_detail::checkActionName(N.view(), help); }
	template <spec_detail::Name M> friend consteval Trigger<M> trigger(std::string_view help);
	std::string_view help_;
};
/// One call: into loop `LoopH`'s state, its arguments checked by `Args` (a SettingsTable); made only by action::call.
template <spec_detail::Name N, const auto& Args, auto LoopH> class Call {
public:
	static constexpr bool isCall = true;
	using ArgsTable = std::remove_cvref_t<decltype(Args)>;
	using Loop = decltype(LoopH);
	constexpr ActionDeclaration declaration() const noexcept {
		return {.name = N.view(),
		        .target = ActionDeclaration::Target::loop,
		        .help = help_,
		        .args = Args.rows(),
		        .table = &Args,
		        .loop = Loop::name};
	}

private:
	consteval explicit Call(std::string_view help) : help_(help) { spec_detail::checkActionName(N.view(), help); }
	template <spec_detail::Name M, const auto& A, auto L> friend consteval Call<M, A, L> call(std::string_view help);
	std::string_view help_;
};
template <class T> inline constexpr bool isAction = false;
template <spec_detail::Name N> inline constexpr bool isAction<Trigger<N>> = true;
template <spec_detail::Name N, const auto& Args, auto LoopH> inline constexpr bool isAction<Call<N, Args, LoopH>> =
	true;

/// Into the node's code, wait-free: no arguments, answered {} once handed over; io.triggered<t>() is true once in the
/// next block. Several invokes before a block are one.
template <spec_detail::Name N> consteval Trigger<N> trigger(std::string_view help) { return Trigger<N>(help); }
/// Into a loop's state, posted to `LoopH` (a loop the spec lists, declared with a state): its arguments checked by
/// `Args`, a SettingsTable; the handler's std::expected<void, std::string> is the reply.
template <spec_detail::Name N, const auto& Args, auto LoopH>
consteval Call<N, Args, LoopH> call(std::string_view help) {
	static_assert(std::derived_from<std::remove_cvref_t<decltype(Args)>, AnySettingsTable>,
	              "a call's arguments are a SettingsTable");
	static_assert(
		!std::remove_cvref_t<decltype(Args)>::anyRowMarked,
		"a call's arguments never change, so they carry no change mark (docs/writing-a-node-type.md#change-marks)");
	static_assert(isLoopHandle<decltype(LoopH)>, "a call names a loop handle");
	static_assert(decltype(LoopH)::stateful, "a call goes to a loop declared with a state");
	return Call<N, Args, LoopH>(help);
}
} // namespace action

/// The tag a call's handler is found by: `std::expected<void, std::string> act(Invoked<forgetPairing>, const
/// CheckedOf<forgetArgs>&)`, a member of the call's loop's state.
template <const auto& Action> struct Invoked {
	static_assert(std::remove_cvref_t<decltype(Action)>::isCall, "Invoked names a call");
};

/// A spec's actions (TypeSpec's `.actions`), in declaration order: unique names, each in a named namespace.
template <const auto&... Actions>
	requires(action::isAction<std::remove_cvref_t<decltype(Actions)>> && ...)
struct ActionTable {
	static_assert((linkage::named<Actions> && ...),
	              "a type's actions are declared in a named namespace, never an unnamed one");
	static constexpr std::size_t count = sizeof...(Actions);
	static_assert(spec_detail::uniqueViews(std::array<std::string_view, count>{Actions.declaration().name...}),
	              "a type's actions have unique names");
	template <const auto& Action> static constexpr bool has =
		((static_cast<const void*>(&Action) == static_cast<const void*>(&Actions)) || ...);
	template <const auto& Action> static constexpr std::size_t indexOf =
		spec_detail::referenceIndex<Action, Actions...>();
	/// How many are triggers: each has one counter in the configuration's realtime channels.
	static constexpr std::size_t triggers = ((std::remove_cvref_t<decltype(Actions)>::isCall ? 0 : 1) + ... + 0);
	/// Each trigger's index among the triggers, in declaration order.
	template <const auto& Action> static constexpr std::size_t triggerIndexOf = [] {
		constexpr std::array<bool, count> trigger{!std::remove_cvref_t<decltype(Actions)>::isCall...};
		std::size_t index = 0;
		for (std::size_t at = 0; at < indexOf<Action>; ++at)
			if (trigger[at]) ++index;
		return index;
	}();
	static constexpr std::array<ActionDeclaration, count> declared = [] {
		std::array<ActionDeclaration, count> all{Actions.declaration()...};
		std::size_t trigger = 0;
		for (ActionDeclaration& one : all)
			if (one.target == ActionDeclaration::Target::code) one.trigger = trigger++;
		return all;
	}();
};
template <class T> inline constexpr bool isActionTable = false;
template <const auto&... Actions> inline constexpr bool isActionTable<ActionTable<Actions...>> = true;

namespace spec_detail {
/// What a call's handler must be, on its loop's state.
template <const auto& Action> struct CallHandler {
	using Declared = std::remove_cvref_t<decltype(Action)>;
	using State = typename Declared::Loop::State;
	using Args = typename Declared::ArgsTable::Checked;
	using Signature = std::expected<void, std::string> (State::*)(Invoked<Action>, const Args&);
	static constexpr bool exact = requires { static_cast<Signature>(&State::act); };
	static_assert(exact, "a call's handler is std::expected<void, std::string> act(Invoked<call>, const "
	                     "CheckedOf<args>&), a member of its loop's state");
	/// On the loop's thread, with the state and the walked arguments.
	static std::expected<void, std::string> run(void* state, CheckedSettings&& args) {
		if constexpr (exact)
			return (static_cast<State*>(state)->*static_cast<Signature>(&State::act))(
				Invoked<Action>{}, ArgsAccess::make<Args>(std::move(args)));
		else return {};
	}
};
/// A call's handler as NodeConfig runs it; null for a trigger.
using CallRunner = std::expected<void, std::string> (*)(void* state, CheckedSettings&& args);
template <class Table> struct CallRunners;
template <const auto&... Actions> struct CallRunners<ActionTable<Actions...>> {
	template <const auto& Action> static constexpr CallRunner one() {
		if constexpr (std::remove_cvref_t<decltype(Action)>::isCall) return &CallHandler<Action>::run;
		else return nullptr;
	}
	static constexpr std::array<CallRunner, sizeof...(Actions)> runners{one<Actions>()...};
};
} // namespace spec_detail
} // namespace ear
