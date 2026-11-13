// The engine's callback boundary (docs/architecture.md#the-callback-boundary): the one way ear gives PipeWire or
// alsa-lib a C function to call. An exception that reaches one unwinds through C frames, which is std::terminate with
// no word of where. Here every callback is one of:
//   - guarded<&Class::member>: noexcept, calls the member and catches; what it caught goes to the Faults the engine was
//     made with (faults.hpp), naming the callback and where the main loop was, then to the object when its class is a
//     FaultOwner. For every loop but the data loop;
//   - realtime<&Class::member>: for the data loop, whose code never throws (docs/engine.md#the-one-data-loop-and-invokes):
//     the member must be noexcept, and nothing is added around it;
//   - realtimeFunction<&function>: a noexcept C function a library calls on any thread (the log, alsa-lib's handler).
// The registration functions below (addEvent, addTimer, addSignal, invoke, listen, listenRealtime, installLog,
// setAlsaErrorHandler) take only those, of the kind their loop allows, so a raw function pointer or the wrong kind does not compile; the
// test ear_callbacks_through_the_boundary refuses any file under src/ but this one's two and tests that hands a library
// a callback itself. Private to ear_pipewire: its sources include it by this path.
#pragma once
#include <algorithm>
#include <array>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <dlfcn.h>

#include <pipewire/data-loop.h>
#include <pipewire/filter.h>
#include <pipewire/impl-module.h>
#include <pipewire/impl-node.h>
#include <pipewire/log.h>
#include <pipewire/pipewire.h>
#include <spa/support/log.h>

#include "ear/pipewire/faults.hpp"

namespace ear::pipewire {

/// An adapter whose state is one node's alone (a block, an engine companion and its body, a link): a throw out of its
/// own callback is its bug, which fails only that node (docs/architecture.md#faults-by-owner). Told inside the boundary,
/// after the Faults, never from the call that threw. A throw that came out of core through its callback (one that
/// carries a place, MainLoopActivity::takeThrown) is core's, and the owner is not told: an owner that tells core
/// anything but NodeHost (which keeps a place itself) opens a MainLoopActivity::Scope around it.
class FaultOwner {
public:
	/// The node the owner's state belongs to, which a fault fails; empty for none (a route's link, which fails its
	/// route).
	virtual std::string_view faultNode() const noexcept = 0;
	/// Main thread: give up what the callback left half-done and tell the node (or route) it is lost, its detail
	/// internalErrorDetail(callback, detail). What this throws (NodeHost's bug, as it is told) is reported as thrown
	/// out of `callback`, with no owner.
	virtual void faulted(std::string_view callback, std::string_view detail) = 0;
	/// The boundary's own: true the first time only. An owner's first fault fails its node; any later throw of its
	/// callbacks (one that keeps throwing) is neither recorded again nor told.
	bool firstFault() noexcept { return !std::exchange(faultTold_, true); }

protected:
	~FaultOwner() = default;

private:
	bool faultTold_ = false;
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

/// Test seam (docs/testing.md#the-tools): the `n`th registration from now (a loop source, a listener, a port block's
/// levels entry), once its owner holds it, throws std::runtime_error("injected after registration <n>"), so a test
/// proves an owner whose constructor threw there left nothing registered; 0 disarms. Never armed outside tests.
void throwAfterRegistration(std::size_t n) noexcept;
/// How many registrations are held now (sources and listeners the boundary made, and port blocks' levels entries):
/// a test's count that an owner whose constructor threw left none behind.
std::size_t registrationsHeld() noexcept;
/// Main thread: a realtime listener's hook and target left on a wedged data loop, taken for the process's life (never
/// freed, as the data loop may still call into them).
void keptForever(void* left) noexcept;
/// How many keptForever took: a test's count that a listener left on a wedged loop was kept, not freed.
std::size_t keptForeverCount() noexcept;
/// Main thread (markDataLoopWedged): the data loop runs no invoke. From then on every step that would wait on it is
/// skipped here, in the one place each such step is taken, and what it would have released is leaked: a blocking
/// invoke into it (invoke with `block`), the removal of a listener bound to it (a filter's: its process callback is
/// cleared by a blocking invoke), and the destruction of a filter or a proxy (a link's releases port mixes by one)
/// (data_loop_wedge.hpp; docs/architecture.md#bounds-the-wedged-data-loop-and-the-watchdog). Cleared when the
/// boundary is next installed.
void markDataLoopWedged() noexcept;
bool dataLoopWedged() noexcept;
/// Test seam: a test that stalled the data loop on purpose and let it go lifts the mark before its objects' teardown,
/// which would otherwise leak what it destroys (as ear does, never freeing anything once wedged).
void clearDataLoopWedged() noexcept;

/// Test seam (docs/testing.md#the-tools): the next guarded callback whose member is `callback`
/// ("FilterPortBlock::onStateChanged") throws std::runtime_error("injected") inside the boundary instead of running,
/// as a bug of that callback's own would; empty disarms. Main thread; never armed outside tests.
void throwInCallback(std::string_view callback);
/// Inside a guarded callback named `callback`, before its member: throws when the seam names it (then disarms).
void injectedIn(std::string_view callback);
/// A registration is now held by its owner: counted, then thrown after when the seam says.
void held();
/// A held registration was removed.
void released() noexcept;

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
template <class Pointer> struct MemberParts;
/// A class that names itself to the boundary: its callbacks are "<boundaryName>::member" in place of its spelling. A
/// class template whose instances are different things to a reader of a report says which one in this (a module-run
/// converter's body names its module's, module_converter_body.hpp).
template <class C>
concept NamesItself = requires {
	{ C::boundaryName } -> std::convertible_to<std::string_view>;
};
template <auto Member> inline constexpr auto spelling = [] {
	constexpr std::string_view name = spelt<Member>();
	using Class = typename MemberParts<decltype(Member)>::Class;
	if constexpr (NamesItself<Class>) {
		constexpr std::string_view owner = Class::boundaryName;
		constexpr std::string_view member = name.substr(name.rfind("::") + 2);
		std::array<char, owner.size() + 2 + member.size()> text{};
		auto out = std::ranges::copy(owner, text.begin()).out;
		out = std::ranges::copy(std::string_view("::"), out).out;
		std::ranges::copy(member, out);
		return text;
	} else {
		std::array<char, name.size()> text{};
		std::ranges::copy(name, text.begin());
		return text;
	}
}();
template <auto Member> inline constexpr std::string_view memberName{spelling<Member>.data(), spelling<Member>.size()};

/// A member function's parts: its class, result, arguments and whether it is noexcept.
template <class C, class R, class... A> struct MemberParts<R (C::*)(A...)> {
	using Class = C;
	using Signature = R(A...);
	static constexpr bool isNoexcept = false;
	static constexpr bool takesNothing = sizeof...(A) == 0;

	template <auto Member> static R guardedCall(void* data, A... args) noexcept {
		auto* self = static_cast<C*>(data);
		entered();
		try {
			injectedIn(memberName<Member>);
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
			boundary::injectedIn(name);
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

/// An engine node's realtime events (pw_impl_node_rt_events), told on the data loop: realtime<> entries only.
template <class C> struct NodeRealtimeCallbacks {
	using Class = C;
	RealtimeSlot<C, void()> start{};      // the driver starts a cycle, before any follower is triggered
	RealtimeSlot<C, void()> incomplete{}; // the running cycle was cut short, told before the next one's start
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

/// A loop source the boundary registered, held by its owner: destroying it (or reset()) removes the source, on the
/// loop's own thread, so a constructor that throws after registering one leaves nothing for the loop to call into a
/// freed object (docs/architecture.md#faults-by-owner). Filled in place by addEvent, addTimer or addSignal; empty when
/// the loop refused it.
class LoopSource {
public:
	LoopSource() = default;
	LoopSource(const LoopSource&) = delete;
	LoopSource& operator=(const LoopSource&) = delete;
	~LoopSource() { reset(); }
	spa_source* get() const noexcept { return source_; }
	explicit operator bool() const noexcept { return source_ != nullptr; }
	void reset() noexcept {
		if (source_ == nullptr) return;
		pw_loop_destroy_source(loop_, source_);
		source_ = nullptr;
		boundary::released();
	}
	/// The registrations' own: holds `source` of `loop` (none when null), then tells the boundary it is held.
	void hold(pw_loop* loop, spa_source* source) {
		reset();
		loop_ = loop;
		source_ = source;
		if (source_ != nullptr) boundary::held();
	}

private:
	pw_loop* loop_ = nullptr;
	spa_source* source_ = nullptr;
};

/// A listener the boundary added, held by its owner: destroying it (or remove()) takes it off its object's list. Its
/// object must outlive it, so an owner declares the object's holder before its listeners, and removes them first in
/// its own destructor (destroying a proxy or a filter leaves its list's memory freed).
class Listener {
public:
	Listener() = default;
	Listener(const Listener&) = delete;
	Listener& operator=(const Listener&) = delete;
	~Listener() { remove(); }
	explicit operator bool() const noexcept { return attached_; }
	/// On a wedged data loop a listener bound to it is left on its object's list, which is leaked with it (and with
	/// its owner), since taking it off would wait on that loop for good.
	void remove() noexcept {
		if (!attached_) return;
		attached_ = false;
		if (!(dataLoopBound_ && boundary::dataLoopWedged())) spa_hook_remove(&hook_);
		boundary::released();
	}
	/// The registrations' own: the hook to attach (detached first), then attached() once it is on the list.
	spa_hook& attach() noexcept {
		remove();
		hook_ = spa_hook{};
		return hook_;
	}
	/// `dataLoopBound`: removing it waits on the data loop (a filter's listener).
	void attached(bool dataLoopBound = false) {
		attached_ = true;
		dataLoopBound_ = dataLoopBound;
		boundary::held();
	}

private:
	spa_hook hook_{};
	bool attached_ = false;
	bool dataLoopBound_ = false;
};

/// A realtime listener the boundary added to an engine node, and the target its callbacks reach, both held by it (never
/// by the owner of the listener): destroying it (or remove()) takes the hook off the node's list on the data loop, a
/// blocking invoke (pw_impl_node_remove_rt_listener), then frees the target. The node must outlive it. On a wedged data
/// loop the removal would wait for good, so the hook is left on the list and the boundary keeps the hook and its
/// target for the process's life, which the data loop may still call into: whoever owns the listener may go at once,
/// and no owner can forget to leak what it reaches (docs/architecture.md#bounds-the-wedged-data-loop-and-the-watchdog).
/// A listener that left one is never attached again.
template <class Target> class RealtimeListener {
public:
	RealtimeListener() = default;
	RealtimeListener(const RealtimeListener&) = delete;
	RealtimeListener& operator=(const RealtimeListener&) = delete;
	~RealtimeListener() { static_cast<void>(remove()); }
	explicit operator bool() const noexcept { return held_ != nullptr; }
	/// The target its callbacks reach; null when nothing is attached.
	Target* target() const noexcept { return held_ != nullptr ? held_->target.get() : nullptr; }
	/// Main thread. Whether the hook was left on the node's list for good (a wedged data loop), kept by the boundary
	/// with its target.
	[[nodiscard]] bool remove() noexcept {
		if (held_ == nullptr) return false;
		const bool left = boundary::dataLoopWedged();
		if (left) {
			left_ = true;
			boundary::keptForever(held_.release()); // never freed: the data loop may still call into it
		} else {
			pw_impl_node_remove_rt_listener(held_->node, &held_->hook);
			held_.reset();
		}
		boundary::released();
		return left;
	}
	/// The registrations' own: holds `target` and a fresh hook for `node` (what it held before removed first), then
	/// attached() once the hook is posted. std::logic_error once a hook was left: the boundary keeps that one.
	spa_hook& attach(pw_impl_node* node, std::unique_ptr<Target> target) {
		if (left_) throw std::logic_error("a realtime listener left on a wedged data loop is not attached again");
		if (remove()) throw std::logic_error("a realtime listener left on a wedged data loop is not attached again");
		held_ = std::make_unique<Held>(node, std::move(target));
		return held_->hook;
	}
	void attached() { boundary::held(); }

private:
	struct Held {
		Held(pw_impl_node* on, std::unique_ptr<Target> reached) : node(on), target(std::move(reached)) {}
		pw_impl_node* node;
		spa_hook hook{};
		std::unique_ptr<Target> target;
	};
	std::unique_ptr<Held> held_;
	bool left_ = false;
};

/// Destroys a proxy ear made (its registry's too: a registry is a proxy); leaks it on a wedged data loop (above).
struct ProxyDestroy {
	void operator()(pw_proxy* proxy) const noexcept {
		if (!boundary::dataLoopWedged()) pw_proxy_destroy(proxy);
	}
	void operator()(pw_registry* registry) const noexcept { (*this)(reinterpret_cast<pw_proxy*>(registry)); }
};
/// Destroys a filter ear made; leaks it on a wedged data loop (above): its destruction removes its node from that
/// loop by a blocking invoke.
struct FilterDestroy {
	void operator()(pw_filter* filter) const noexcept {
		if (!boundary::dataLoopWedged()) pw_filter_destroy(filter);
	}
};
using OwnedFilter = std::unique_ptr<pw_filter, FilterDestroy>;
/// A proxy held by its owner, declared before the listeners on it, so it goes after them, a constructor's unwinding
/// included.
using OwnedProxy = std::unique_ptr<pw_proxy, ProxyDestroy>;
using OwnedRegistry = std::unique_ptr<pw_registry, ProxyDestroy>;

// The only registrations ear makes. Sources (events, timers, signals) live on loops that may run anything (the main
// loop, a sink's or handoff's thread loop) and take only guarded<>; the data loop takes only realtime<>. Each fills the
// owner's LoopSource or Listener in place.

/// An event source on `loop`: the member takes the count of signals since it last ran.
template <class B>
	requires(isGuarded<B> && BoundaryCallback<B, void(std::uint64_t)>)
void addEvent(LoopSource& source, pw_loop* loop, B /*callback*/, typename B::Class* self) {
	source.hold(loop, pw_loop_add_event(loop, B::call, self));
}
/// A timer source on `loop` (armed with pw_loop_update_timer): the member takes the expirations.
template <class B>
	requires(isGuarded<B> && BoundaryCallback<B, void(std::uint64_t)>)
void addTimer(LoopSource& source, pw_loop* loop, B /*callback*/, typename B::Class* self) {
	source.hold(loop, pw_loop_add_timer(loop, B::call, self));
}
/// A signal source on `loop`: the member takes the signal's number.
template <class B>
	requires(isGuarded<B> && BoundaryCallback<B, void(int)>)
void addSignal(LoopSource& source, pw_loop* loop, int signal, B /*callback*/, typename B::Class* self) {
	source.hold(loop, pw_loop_add_signal(loop, signal, B::call, self));
}
/// An invoke on a loop that is not the data loop: guarded. The member takes nothing and returns int or nothing (0).
template <class B>
	requires isGuarded<B>
int invoke(pw_loop* loop, B /*callback*/, typename B::Class* self, bool block) {
	return pw_loop_invoke(loop, &B::invoked, 0, nullptr, 0, block, self);
}
/// An invoke on the data loop: realtime only. The member takes nothing and returns int or nothing (0). A blocking one
/// on a wedged data loop is never made: -EDEADLK, and nothing ran (above).
template <class B>
	requires isRealtime<B>
int invoke(DataLoop loop, B /*callback*/, typename B::Class* self, bool block) {
	if (block && boundary::dataLoopWedged()) return -EDEADLK;
	return pw_loop_invoke(loop.loop(), &B::invoked, 0, nullptr, 0, block, self);
}

/// A filter's events.
template <auto Table>
	requires isTable<decltype(Table), FilterCallbacks>
void listen(pw_filter* filter, Listener& listener, typename decltype(Table)::Class* self) {
	static constexpr pw_filter_events events = [] {
		pw_filter_events table{};
		table.version = PW_VERSION_FILTER_EVENTS;
		table.state_changed = Table.stateChanged.function;
		table.param_changed = Table.paramChanged.function;
		table.process = Table.process.function;
		return table;
	}();
	pw_filter_add_listener(filter, &listener.attach(), &events, self);
	listener.attached(true);
}
/// A proxy's own events (removed, error).
template <auto Table>
	requires isTable<decltype(Table), ProxyCallbacks>
void listen(pw_proxy* proxy, Listener& listener, typename decltype(Table)::Class* self) {
	static constexpr pw_proxy_events events = [] {
		pw_proxy_events table{};
		table.version = PW_VERSION_PROXY_EVENTS;
		table.removed = Table.removed.function;
		table.error = Table.error.function;
		return table;
	}();
	pw_proxy_add_listener(proxy, &listener.attach(), &events, self);
	listener.attached();
}
/// A node proxy's object events.
template <auto Table>
	requires isTable<decltype(Table), NodeCallbacks>
void listen(pw_proxy* node, Listener& listener, typename decltype(Table)::Class* self) {
	static constexpr pw_node_events events = [] {
		pw_node_events table{};
		table.version = PW_VERSION_NODE_EVENTS;
		table.info = Table.info.function;
		return table;
	}();
	pw_proxy_add_object_listener(node, &listener.attach(), &events, self);
	listener.attached();
}
/// A link proxy's object events.
template <auto Table>
	requires isTable<decltype(Table), LinkCallbacks>
void listen(pw_proxy* link, Listener& listener, typename decltype(Table)::Class* self) {
	static constexpr pw_link_events events = [] {
		pw_link_events table{};
		table.version = PW_VERSION_LINK_EVENTS;
		table.info = Table.info.function;
		return table;
	}();
	pw_proxy_add_object_listener(link, &listener.attach(), &events, self);
	listener.attached();
}
/// A registry's events.
template <auto Table>
	requires isTable<decltype(Table), RegistryCallbacks>
void listen(pw_registry* registry, Listener& listener, typename decltype(Table)::Class* self) {
	static constexpr pw_registry_events events = [] {
		pw_registry_events table{};
		table.version = PW_VERSION_REGISTRY_EVENTS;
		table.global = Table.global.function;
		table.global_remove = Table.globalRemove.function;
		return table;
	}();
	pw_registry_add_listener(registry, &listener.attach(), &events, self);
	listener.attached();
}
/// The core's events.
template <auto Table>
	requires isTable<decltype(Table), CoreCallbacks>
void listen(pw_core* core, Listener& listener, typename decltype(Table)::Class* self) {
	static constexpr pw_core_events events = [] {
		pw_core_events table{};
		table.version = PW_VERSION_CORE_EVENTS;
		table.done = Table.done.function;
		return table;
	}();
	pw_core_add_listener(core, &listener.attach(), &events, self);
	listener.attached();
}
/// A loaded module's events.
template <auto Table>
	requires isTable<decltype(Table), ModuleCallbacks>
void listen(pw_impl_module* module, Listener& listener, typename decltype(Table)::Class* self) {
	static constexpr pw_impl_module_events events = [] {
		pw_impl_module_events table{};
		table.version = PW_VERSION_IMPL_MODULE_EVENTS;
		table.destroy = Table.destroy.function;
		return table;
	}();
	pw_impl_module_add_listener(module, &listener.attach(), &events, self);
	listener.attached();
}

/// An engine node's realtime events, on the data loop (the internal clock's: docs/engine.md#graphcycle), each called on
/// `target`, which the listener holds from here on. The hook is added by an asynchronous invoke on that loop, so it
/// hears every cycle a node made after this call runs.
template <auto Table>
	requires isTable<decltype(Table), NodeRealtimeCallbacks>
void listenRealtime(pw_impl_node* node, RealtimeListener<typename decltype(Table)::Class>& listener,
                    std::unique_ptr<typename decltype(Table)::Class> target) {
	static constexpr pw_impl_node_rt_events events = [] {
		pw_impl_node_rt_events table{};
		table.version = PW_VERSION_IMPL_NODE_RT_EVENTS;
		table.start = Table.start.function;
		table.incomplete = Table.incomplete.function;
		return table;
	}();
	spa_hook& hook = listener.attach(node, std::move(target));
	pw_impl_node_add_rt_listener(node, &hook, &events, listener.target());
	listener.attached();
}

/// Main thread: the engine node of this process's context named `name` (node.name), or null. PipeWire walks its
/// globals with a function of the boundary's own, which cannot throw.
pw_impl_node* engineNode(pw_context* context, std::string_view name) noexcept;

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
