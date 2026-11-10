// The engine's callback boundary (docs/architecture.md#the-callback-boundary): the one way ear gives PipeWire or
// alsa-lib a C function to call. An exception that reaches one unwinds through C frames, which is std::terminate with
// no word of where. Here every callback is one of:
//   - guarded<&Class::member>: noexcept, calls the member and catches; what it caught goes to the Faults the engine was
//     made with (faults.hpp), naming the callback and where the main loop was, then to the object when its class is a
//     FaultOwner. For every loop but the data loop;
//   - realtime<&Class::member>: for the data loop, whose code never throws (docs/engine.md#the-one-data-loop-and-invokes):
//     the member must be noexcept, and nothing is added around it;
//   - realtimeFunction<&function>: a noexcept C function a library calls on any thread (the log, alsa-lib's handler).
// The registration functions below (addEvent, addTimer, addSignal, invoke, listen, installLog, setAlsaErrorHandler)
// take only those, of the kind their loop allows, so a raw function pointer or the wrong kind does not compile; the
// test ear_callbacks_through_the_boundary refuses any file under src/ but this one's two and tests that hands a library
// a callback itself. Private to ear_pipewire: its sources include it by this path.
#pragma once
#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include <dlfcn.h>

#include <pipewire/data-loop.h>
#include <pipewire/filter.h>
#include <pipewire/impl-module.h>
#include <pipewire/log.h>
#include <pipewire/pipewire.h>
#include <spa/support/log.h>

#include "ear/pipewire/faults.hpp"

namespace ear::pipewire {

/// An adapter whose state is one node's alone (a block, an engine companion and its body, a link): a throw out of its
/// own callback is its bug, which fails only that node (docs/architecture.md#faults-by-owner). Told inside the boundary,
/// after the Faults, never from the call that threw. A throw that came out of NodeHost through its callback (one that
/// carries a place, MainLoopActivity::takeThrown) is NodeHost's, and the owner is not told.
class FaultOwner {
public:
	/// The node the owner's state belongs to, which a fault fails; empty for none (a route's link, which fails its
	/// route).
	virtual std::string_view faultNode() const noexcept = 0;
	/// Main thread: give up what the callback left half-done and tell the node (or route) it is lost, its detail
	/// internalErrorDetail(callback, detail). What this throws (NodeHost's bug, as it is told) is reported as thrown
	/// out of `callback`, with no owner.
	virtual void faulted(std::string_view callback, std::string_view detail) = 0;

protected:
	~FaultOwner() = default;
};

namespace boundary {

/// Main thread (Engine::create): where every caught exception is reported from now on; null reports to the boundary's
/// own last words (a line on standard error, then std::terminate). The calling thread is the main loop's. False, with
/// nothing changed, while another holds the boundary: one engine at a time, since a trampoline carries only its object
/// and so reports to the process's one Faults.
bool install(Faults* faults) noexcept;
/// Main thread (~Engine), by the one install() let in: nothing installed.
void uninstall() noexcept;
/// A callback begins: on the main thread and with no exception unwinding, a throw's place left by a throw handled
/// before it is forgotten.
void entered() noexcept;
/// Inside a catch handler: the exception being handled, reported as thrown out of `callback`, then told to `owner`
/// unless it came out of NodeHost.
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
template <class C, class Signature, bool RealtimeOnly> class CallbackSlot;
template <class C, class R, class... A, bool RealtimeOnly> class CallbackSlot<C, R(A...), RealtimeOnly> {
public:
	using Function = R (*)(void*, A...);
	constexpr CallbackSlot() = default;
	template <auto Member>
		requires(!RealtimeOnly && std::same_as<MemberClass<Member>, C> &&
	             std::same_as<typename Guarded<Member>::Signature, R(A...)>)
	constexpr CallbackSlot(Guarded<Member>) : function(Guarded<Member>::call) {}
	template <auto Member>
		requires(RealtimeOnly && std::same_as<MemberClass<Member>, C> &&
	             std::same_as<typename Realtime<Member>::Signature, R(A...)>)
	constexpr CallbackSlot(Realtime<Member>) : function(Realtime<Member>::call) {}
	const Function function = nullptr; // public and const: a table is a template argument (a structural type)
};
template <class C, class Signature> using GuardedSlot = CallbackSlot<C, Signature, false>;
template <class C, class Signature> using RealtimeSlot = CallbackSlot<C, Signature, true>;

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

/// The engine's one data loop, as a type of its own, so what runs on it is chosen by type: only a realtime<> callback
/// is invoked on it. Made from the context's pw_data_loop, or from a filter (pw_filter_get_data_loop: the loop its
/// process runs on, null when it has none).
class DataLoop {
public:
	explicit DataLoop(pw_data_loop* loop) noexcept : loop_(loop != nullptr ? pw_data_loop_get_loop(loop) : nullptr) {}
	static DataLoop of(pw_filter* filter) noexcept { return DataLoop(pw_filter_get_data_loop(filter)); }
	constexpr DataLoop() = default;
	pw_loop* loop() const noexcept { return loop_; }
	explicit operator bool() const noexcept { return loop_ != nullptr; }
	friend bool operator==(DataLoop, DataLoop) = default;

private:
	explicit DataLoop(pw_loop* loop) noexcept : loop_(loop) {}
	pw_loop* loop_ = nullptr;
};

// The only registrations ear makes. Sources (events, timers, signals) live on loops that may run anything (the main
// loop, a sink's or handoff's thread loop) and take only guarded<>; the data loop takes only realtime<>.

/// An event source on `loop`: the member takes the count of signals since it last ran.
template <class B>
	requires(isGuarded<B> && BoundaryCallback<B, void(std::uint64_t)>)
spa_source* addEvent(pw_loop* loop, B /*callback*/, typename B::Class* self) {
	return pw_loop_add_event(loop, B::call, self);
}
/// A timer source on `loop` (armed with pw_loop_update_timer): the member takes the expirations.
template <class B>
	requires(isGuarded<B> && BoundaryCallback<B, void(std::uint64_t)>)
spa_source* addTimer(pw_loop* loop, B /*callback*/, typename B::Class* self) {
	return pw_loop_add_timer(loop, B::call, self);
}
/// A signal source on `loop`: the member takes the signal's number.
template <class B>
	requires(isGuarded<B> && BoundaryCallback<B, void(int)>)
spa_source* addSignal(pw_loop* loop, int signal, B /*callback*/, typename B::Class* self) {
	return pw_loop_add_signal(loop, signal, B::call, self);
}
/// An invoke on a loop that is not the data loop: guarded. The member takes nothing and returns int or nothing (0).
template <class B>
	requires isGuarded<B>
int invoke(pw_loop* loop, B /*callback*/, typename B::Class* self, bool block) {
	return pw_loop_invoke(loop, &B::invoked, 0, nullptr, 0, block, self);
}
/// An invoke on the data loop: realtime only. The member takes nothing and returns int or nothing (0).
template <class B>
	requires isRealtime<B>
int invoke(DataLoop loop, B /*callback*/, typename B::Class* self, bool block) {
	return pw_loop_invoke(loop.loop(), &B::invoked, 0, nullptr, 0, block, self);
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

// Plain C functions ear hands a library as an interface's methods or a handler: PipeWire's log and alsa-lib's error
// handler, called on any thread, the data loop among them, with C varargs no trampoline can forward. They are
// realtime: the function itself must be noexcept, and nothing is added.

template <class T> inline constexpr bool isNoexceptFunction = false;
template <class R, class... A> inline constexpr bool isNoexceptFunction<R (*)(A...) noexcept> = true;
template <class R, class... A> inline constexpr bool isNoexceptFunction<R (*)(A..., ...) noexcept> = true;

/// A noexcept function of ours, as a value only realtimeFunction<> makes.
template <auto Function> class RealtimeFunction {
	static_assert(isNoexceptFunction<decltype(Function)>,
	              "a function a library calls on any thread must be noexcept: the data loop never throws");

public:
	static constexpr auto function = Function;
};
template <auto Function> inline constexpr RealtimeFunction<Function> realtimeFunction{};

/// One entry of a methods table: a RealtimeFunction whose type is the entry's, noexcept.
template <class Pointer> class FunctionSlot {
public:
	constexpr FunctionSlot() = default;
	template <auto Function>
		requires std::is_convertible_v<decltype(Function), Pointer>
	constexpr FunctionSlot(RealtimeFunction<Function>) : function(Function) {}
	const Pointer function = nullptr; // public and const: a table is a template argument (a structural type)
};

/// PipeWire's log interface (spa_log_methods), every method a realtime function.
struct LogCallbacks {
	FunctionSlot<decltype(spa_log_methods::log)> log{};
	FunctionSlot<decltype(spa_log_methods::logv)> logv{};
	FunctionSlot<decltype(spa_log_methods::logt)> logt{};
	FunctionSlot<decltype(spa_log_methods::logtv)> logtv{};
	FunctionSlot<decltype(spa_log_methods::topic_init)> topicInit{};
};

/// Makes the table PipeWire's process-wide log, at `level`: one spa_log per table, for the process's life.
template <LogCallbacks Table> void installLog(spa_log_level level) {
	static constexpr spa_log_methods methods = [] {
		spa_log_methods table{};
		table.version = SPA_VERSION_LOG_METHODS;
		table.log = Table.log.function;
		table.logv = Table.logv.function;
		table.logt = Table.logt.function;
		table.logtv = Table.logtv.function;
		table.topic_init = Table.topicInit.function;
		return table;
	}();
	static spa_log log{.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Log, SPA_VERSION_LOG, &methods, nullptr),
	                   .level = level};
	pw_log_set(&log);
}

/// alsa-lib's error handler (snd_lib_error_handler_t).
using AlsaErrorHandler = void (*)(const char* file, int line, const char* function, int error, const char* format, ...);

/// Points alsa-lib's process-wide error handler at a realtime function. The image carries alsa-lib's run-time library
/// only (the ALSA plugin links it), so it is reached by name, not linked; where it is missing there is no ALSA to log.
template <auto Function>
	requires std::is_convertible_v<decltype(Function), AlsaErrorHandler>
void setAlsaErrorHandler(RealtimeFunction<Function> /*handler*/) {
	using SetHandler = int (*)(AlsaErrorHandler);
	static void* const library = ::dlopen("libasound.so.2", RTLD_NOW | RTLD_GLOBAL);
	if (library == nullptr) return;
	if (auto* set = reinterpret_cast<SetHandler>(::dlsym(library, "snd_lib_error_set_handler"))) set(Function);
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
