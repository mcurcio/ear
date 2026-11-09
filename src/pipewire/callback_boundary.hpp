// The engine's callback boundary (docs/architecture.md#the-callback-boundary): the one way ear gives PipeWire a C
// callback. PipeWire calls ear through C function pointers, and an exception that reaches one unwinds through C frames,
// which is std::terminate with no word of where. Here every callback is a member function wrapped in a trampoline:
//   - guarded<&Class::member>: noexcept, calls the member and catches; what it caught goes to the Faults the engine was
//     made with (faults.hpp), naming the callback and where the main loop was, then to the object when its class is a
//     FaultOwner;
//   - realtime<&Class::member>: for the data loop, whose code never throws (docs/engine.md#the-one-data-loop-and-invokes):
//     the member must be noexcept, and nothing is added around it.
// The registration functions below (addEvent, addTimer, addSignal, invoke, listen) take only those, so a raw function
// pointer does not compile; the test ear_callbacks_through_the_boundary refuses any file under src/ but this one that
// calls PipeWire's registrations itself. Private to ear_pipewire: its sources include it by this path.
#pragma once
#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include <pipewire/filter.h>
#include <pipewire/impl-module.h>
#include <pipewire/pipewire.h>

#include "ear/pipewire/faults.hpp"

namespace ear::pipewire {

/// An object whose own callback may fault: told when one threw, inside the boundary, never from the call that threw.
class FaultOwner {
public:
	virtual void faulted(std::string_view callback, std::string_view detail) noexcept = 0;

protected:
	~FaultOwner() = default;
};

namespace boundary {

/// Main thread (Engine::create): where every caught exception is reported from now on; null reports to the boundary's
/// own last words (a line on standard error, then std::terminate). The calling thread is the main loop's.
void install(Faults* faults) noexcept;
/// Main thread (~Engine): nothing installed.
void uninstall() noexcept;
/// A callback begins: on the main thread, a throw's place left by a throw handled before it is forgotten.
void entered() noexcept;
/// Inside a catch handler: the exception being handled, reported as thrown out of `callback`, then told to `owner`.
void report(std::string_view callback, FaultOwner* owner) noexcept;

/// The member a pointer names, as "Class::member" (namespaces ear::pipewire and anonymous left out), made at compile
/// time from the compiler's own spelling of the template argument.
template <auto Member> consteval std::string_view spelt() {
	constexpr std::string_view pretty = __PRETTY_FUNCTION__;
	constexpr std::string_view marker = "Member = &";
	std::string_view name = pretty.substr(pretty.find(marker) + marker.size());
	name = name.substr(0, name.find_first_of(";]"));
	for (bool trimmed = true; trimmed;) {
		trimmed = false;
		for (const std::string_view prefix : {"ear::pipewire::", "{anonymous}::", "(anonymous namespace)::"}) {
			if (!name.starts_with(prefix)) continue;
			name.remove_prefix(prefix.size());
			trimmed = true;
		}
	}
	return name;
}
template <auto Member> inline constexpr auto spelling = [] {
	constexpr std::string_view name = spelt<Member>();
	std::array<char, name.size()> text{};
	std::ranges::copy(name, text.begin());
	return text;
}();
template <auto Member> inline constexpr std::string_view memberName{spelling<Member>.data(), spelling<Member>.size()};

/// A member function's parts: its class, result, arguments and whether it is noexcept.
template <class Pointer> struct MemberParts;
template <class C, class R, class... A> struct MemberParts<R (C::*)(A...)> {
	using Class = C;
	using Signature = R(A...);
	static constexpr bool isNoexcept = false;
	static constexpr bool takesNothing = sizeof...(A) == 0;

	template <auto Member> static R guardedCall(void* data, A... args) noexcept {
		auto* self = static_cast<C*>(data);
		entered();
		try {
			return (self->*Member)(args...);
		} catch (...) {
			if constexpr (std::is_base_of_v<FaultOwner, C>) report(memberName<Member>, self);
			else report(memberName<Member>, nullptr);
			if constexpr (!std::is_void_v<R>) return R{};
		}
	}
};
template <class C, class R, class... A> struct MemberParts<R (C::*)(A...) noexcept> : MemberParts<R (C::*)(A...)> {
	static constexpr bool isNoexcept = true;

	template <auto Member> static R realtimeCall(void* data, A... args) noexcept {
		return (static_cast<C*>(data)->*Member)(args...);
	}
};

/// A loop's invoke (spa_invoke_func_t), user data last, calling a member that takes nothing; int or void.
template <class Call> int invokeResult(Call&& call) {
	if constexpr (std::is_void_v<std::invoke_result_t<Call>>) {
		call();
		return 0;
	} else {
		return call();
	}
}

} // namespace boundary

/// The class a member pointer belongs to.
template <auto Member> using MemberClass = typename boundary::MemberParts<decltype(Member)>::Class;

/// A C callback made from a member function: noexcept, catching, reporting (above). It holds nothing but its member, so
/// every Guarded is the boundary's own; guarded<> names one.
template <auto Member> class Guarded {
	using Parts = boundary::MemberParts<decltype(Member)>;

public:
	using Class = typename Parts::Class;
	using Signature = typename Parts::Signature;
	static constexpr std::string_view name = boundary::memberName<Member>;
	/// The data-first C function: (void* data, the member's arguments...).
	static constexpr auto call = &Parts::template guardedCall<Member>;
	/// The loop invoke: (spa_loop*, bool, uint32_t, const void*, size_t, void* user data), for a member that takes
	/// nothing.
	static int invoked(spa_loop* /*loop*/, bool /*async*/, std::uint32_t /*seq*/, const void* /*data*/,
	                   std::size_t /*size*/, void* userData) noexcept
		requires Parts::takesNothing
	{
		auto* self = static_cast<Class*>(userData);
		boundary::entered();
		try {
			return boundary::invokeResult([self] { return (self->*Member)(); });
		} catch (...) {
			if constexpr (std::is_base_of_v<FaultOwner, Class>) boundary::report(name, self);
			else boundary::report(name, nullptr);
			return 0;
		}
	}
};

/// A data-loop callback from a noexcept member: nothing is added around it, and a throw is std::terminate, as on the
/// data loop it must be.
template <auto Member> class Realtime {
	using Parts = boundary::MemberParts<decltype(Member)>;
	static_assert(Parts::isNoexcept, "a realtime callback's member must be noexcept: the data loop never throws");

public:
	using Class = typename Parts::Class;
	using Signature = typename Parts::Signature;
	static constexpr std::string_view name = boundary::memberName<Member>;
	static constexpr auto call = &Parts::template realtimeCall<Member>;
	static int invoked(spa_loop* /*loop*/, bool /*async*/, std::uint32_t /*seq*/, const void* /*data*/,
	                   std::size_t /*size*/, void* userData) noexcept
		requires Parts::takesNothing
	{
		auto* self = static_cast<Class*>(userData);
		return boundary::invokeResult([self]() noexcept { return (self->*Member)(); });
	}
};

template <auto Member> inline constexpr Guarded<Member> guarded{};
template <auto Member> inline constexpr Realtime<Member> realtime{};

template <class T> inline constexpr bool isGuarded = false;
template <auto Member> inline constexpr bool isGuarded<Guarded<Member>> = true;
template <class T> inline constexpr bool isRealtime = false;
template <auto Member> inline constexpr bool isRealtime<Realtime<Member>> = true;

/// A callback the boundary made, of this signature (the member's, without the C function's data pointer).
template <class B, class Signature>
concept BoundaryCallback = (isGuarded<B> || isRealtime<B>) && std::same_as<typename B::Signature, Signature>;

/// One entry of an events table: the trampoline of a Guarded (or, for a data-loop entry, a Realtime) whose member
/// belongs to C and has this signature. It holds nothing else, and nothing else converts to it.
template <class C, class Signature, bool RealtimeOnly> class Slot;
template <class C, class R, class... A, bool RealtimeOnly> class Slot<C, R(A...), RealtimeOnly> {
public:
	using Function = R (*)(void*, A...);
	constexpr Slot() = default;
	template <auto Member>
		requires(!RealtimeOnly && std::same_as<MemberClass<Member>, C> &&
	             std::same_as<typename Guarded<Member>::Signature, R(A...)>)
	constexpr Slot(Guarded<Member>) : function(Guarded<Member>::call) {}
	template <auto Member>
		requires(RealtimeOnly && std::same_as<MemberClass<Member>, C> &&
	             std::same_as<typename Realtime<Member>::Signature, R(A...)>)
	constexpr Slot(Realtime<Member>) : function(Realtime<Member>::call) {}
	const Function function = nullptr; // public and const: a table is a template argument (a structural type)
};
template <class C, class Signature> using GuardedSlot = Slot<C, Signature, false>;
template <class C, class Signature> using RealtimeSlot = Slot<C, Signature, true>;

// The events tables ear listens to, one per PipeWire events struct, with the entries ear uses. Each is a template
// argument of listen(), so its PipeWire table is a constant of its own.
template <class C> struct FilterCallbacks {
	using Class = C;
	GuardedSlot<C, void(pw_filter_state, pw_filter_state, const char*)> stateChanged{};
	GuardedSlot<C, void(void*, std::uint32_t, const spa_pod*)> paramChanged{};
	RealtimeSlot<C, void(spa_io_position*)> process{}; // the data loop
};
template <class C> struct ProxyCallbacks {
	using Class = C;
	GuardedSlot<C, void()> removed{};
	GuardedSlot<C, void(int, int, const char*)> error{};
};
template <class C> struct RegistryCallbacks {
	using Class = C;
	GuardedSlot<C, void(std::uint32_t, std::uint32_t, const char*, std::uint32_t, const spa_dict*)> global{};
	GuardedSlot<C, void(std::uint32_t)> globalRemove{};
};
template <class C> struct CoreCallbacks {
	using Class = C;
	GuardedSlot<C, void(std::uint32_t, int)> done{};
};
template <class C> struct ModuleCallbacks {
	using Class = C;
	GuardedSlot<C, void()> destroy{};
};
template <class C> struct NodeCallbacks {
	using Class = C;
	GuardedSlot<C, void(const pw_node_info*)> info{};
};
template <class C> struct LinkCallbacks {
	using Class = C;
	GuardedSlot<C, void(const pw_link_info*)> info{};
};

template <class T, template <class> class Table> inline constexpr bool isTable = false;
template <class C, template <class> class Table> inline constexpr bool isTable<Table<C>, Table> = true;

// The only registrations ear makes.

/// An event source on `loop`: the member takes the count of signals since it last ran.
template <class B>
	requires BoundaryCallback<B, void(std::uint64_t)>
spa_source* addEvent(pw_loop* loop, B /*callback*/, typename B::Class* self) {
	return pw_loop_add_event(loop, B::call, self);
}
/// A timer source on `loop` (armed with pw_loop_update_timer): the member takes the expirations.
template <class B>
	requires BoundaryCallback<B, void(std::uint64_t)>
spa_source* addTimer(pw_loop* loop, B /*callback*/, typename B::Class* self) {
	return pw_loop_add_timer(loop, B::call, self);
}
/// A signal source on `loop`: the member takes the signal's number.
template <class B>
	requires BoundaryCallback<B, void(int)>
spa_source* addSignal(pw_loop* loop, int signal, B /*callback*/, typename B::Class* self) {
	return pw_loop_add_signal(loop, signal, B::call, self);
}
/// An invoke on `loop` (pw_loop_invoke: a data loop's through pw_data_loop_get_loop): the member takes nothing and
/// returns int or nothing (0).
template <class B>
	requires(isGuarded<B> || isRealtime<B>)
int invoke(pw_loop* loop, B /*callback*/, typename B::Class* self, bool block) {
	return pw_loop_invoke(loop, &B::invoked, 0, nullptr, 0, block, self);
}

/// A filter's events.
template <auto Table>
	requires isTable<decltype(Table), FilterCallbacks>
void listen(pw_filter* filter, spa_hook& hook, typename decltype(Table)::Class* self) {
	static constexpr pw_filter_events events = [] {
		pw_filter_events table{};
		table.version = PW_VERSION_FILTER_EVENTS;
		table.state_changed = Table.stateChanged.function;
		table.param_changed = Table.paramChanged.function;
		table.process = Table.process.function;
		return table;
	}();
	pw_filter_add_listener(filter, &hook, &events, self);
}
/// A proxy's own events (removed, error).
template <auto Table>
	requires isTable<decltype(Table), ProxyCallbacks>
void listen(pw_proxy* proxy, spa_hook& hook, typename decltype(Table)::Class* self) {
	static constexpr pw_proxy_events events = [] {
		pw_proxy_events table{};
		table.version = PW_VERSION_PROXY_EVENTS;
		table.removed = Table.removed.function;
		table.error = Table.error.function;
		return table;
	}();
	pw_proxy_add_listener(proxy, &hook, &events, self);
}
/// A node proxy's object events.
template <auto Table>
	requires isTable<decltype(Table), NodeCallbacks>
void listen(pw_proxy* node, spa_hook& hook, typename decltype(Table)::Class* self) {
	static constexpr pw_node_events events = [] {
		pw_node_events table{};
		table.version = PW_VERSION_NODE_EVENTS;
		table.info = Table.info.function;
		return table;
	}();
	pw_proxy_add_object_listener(node, &hook, &events, self);
}
/// A link proxy's object events.
template <auto Table>
	requires isTable<decltype(Table), LinkCallbacks>
void listen(pw_proxy* link, spa_hook& hook, typename decltype(Table)::Class* self) {
	static constexpr pw_link_events events = [] {
		pw_link_events table{};
		table.version = PW_VERSION_LINK_EVENTS;
		table.info = Table.info.function;
		return table;
	}();
	pw_proxy_add_object_listener(link, &hook, &events, self);
}
/// A registry's events.
template <auto Table>
	requires isTable<decltype(Table), RegistryCallbacks>
void listen(pw_registry* registry, spa_hook& hook, typename decltype(Table)::Class* self) {
	static constexpr pw_registry_events events = [] {
		pw_registry_events table{};
		table.version = PW_VERSION_REGISTRY_EVENTS;
		table.global = Table.global.function;
		table.global_remove = Table.globalRemove.function;
		return table;
	}();
	pw_registry_add_listener(registry, &hook, &events, self);
}
/// The core's events.
template <auto Table>
	requires isTable<decltype(Table), CoreCallbacks>
void listen(pw_core* core, spa_hook& hook, typename decltype(Table)::Class* self) {
	static constexpr pw_core_events events = [] {
		pw_core_events table{};
		table.version = PW_VERSION_CORE_EVENTS;
		table.done = Table.done.function;
		return table;
	}();
	pw_core_add_listener(core, &hook, &events, self);
}
/// A loaded module's events.
template <auto Table>
	requires isTable<decltype(Table), ModuleCallbacks>
void listen(pw_impl_module* module, spa_hook& hook, typename decltype(Table)::Class* self) {
	static constexpr pw_impl_module_events events = [] {
		pw_impl_module_events table{};
		table.version = PW_VERSION_IMPL_MODULE_EVENTS;
		table.destroy = Table.destroy.function;
		return table;
	}();
	pw_impl_module_add_listener(module, &hook, &events, self);
}

/// Work the boundary runs that is not a callback of its own (each item the main-loop executor runs): called and
/// caught as a guarded callback is, named `callback`, so one item's throw is reported as its own and the next item
/// still runs once the report returns.
template <class Work> void shield(std::string_view callback, Work& work) noexcept {
	boundary::entered();
	try {
		work();
	} catch (...) {
		boundary::report(callback, nullptr);
	}
}

} // namespace ear::pipewire
