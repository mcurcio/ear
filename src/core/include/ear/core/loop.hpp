// A node's loops (docs/writing-a-node-type.md#loops): serial executors for the node's work off the realtime thread and
// the main loop, declared in its spec, each with its state, alive for the node's whole configuration. What a type
// declares and writes against (loop::strand, loop::dedicated, Loops, Loop) and what the framework runs a loop with
// (NodeLoop, LoopEvents, LoopAdapter: ear_runtime's Loops implements it on real threads, the harness on the test's).
// NodeHost stages what is sent to a loop until the node is configured, gives a loop's state its loop and destroys it
// there, and counts the sources each loop's handlers left armed. Pure: nothing but the standard library and ear_core.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include "ear/core/executor.hpp"
#include "ear/core/graph_spec.hpp"
#include "ear/core/ready_events.hpp"
#include "ear/core/source.hpp"
#include "ear/core/timers.hpp"
namespace ear {

/// Where a loop's handlers run: on the framework's strand pool, never blocking (the default), or on a thread of its own,
/// which may block (a library's poll, a disk write, a source write with patience).
enum class LoopKind { strand, dedicated };
std::string_view loopKindName(LoopKind kind) noexcept; // "strand" | "dedicated"

/// A loop with no state: an executor other loops' states use (cx.executor).
struct NoState {};

namespace spec_detail {
/// A loop's name as a template argument, so each loop of a spec is its own type.
template <std::size_t Size> struct LoopName {
	char text[Size]{};
	consteval LoopName(const char (&name)[Size]) { // NOLINT: implicit on purpose, from a string literal
		std::copy_n(name, Size, text);
	}
	constexpr std::string_view view() const noexcept { return {text, Size - 1}; }
};
/// 1 to 8 of [a-z0-9-]: short enough that its thread's name, "ear-<name> <node>", keeps some of the node's.
consteval bool validLoopName(std::string_view name) {
	return !name.empty() && name.size() <= 8 && std::ranges::all_of(name, [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
	});
}
} // namespace spec_detail

/// One loop of a type, as a compile-time constant: its name, its kind and its state's type (NoState for none). Made only
/// by loop::strand and loop::dedicated.
template <spec_detail::LoopName Name, LoopKind Kind, class StateType> struct LoopHandle {
	static_assert(spec_detail::validLoopName(Name.view()), "a loop's name is 1 to 8 of [a-z0-9-]");
	static_assert(std::is_class_v<StateType> && !std::is_const_v<StateType>, "a loop's state is a class");
	static constexpr std::string_view name = Name.view();
	static constexpr LoopKind kind = Kind;
	using State = StateType;
	static constexpr bool stateful = !std::same_as<StateType, NoState>;
};
template <class T> inline constexpr bool isLoopHandle = false;
template <spec_detail::LoopName Name, LoopKind Kind, class State>
inline constexpr bool isLoopHandle<LoopHandle<Name, Kind, State>> = true;

namespace loop {
/// A loop on the framework's strand pool: its handlers never block.
template <spec_detail::LoopName Name, class State = NoState>
inline constexpr LoopHandle<Name, LoopKind::strand, State> strand{};
/// A loop on a thread of its own, named "ear-<name> <node>" (cut to 15 bytes): its handlers may block.
template <spec_detail::LoopName Name, class State = NoState>
inline constexpr LoopHandle<Name, LoopKind::dedicated, State> dedicated{};
} // namespace loop

/// One declared loop, as the framework reads it.
struct LoopDeclaration {
	std::string_view name;
	LoopKind kind;
	bool stateful;
};

/// A spec's loops (TypeSpec's `.loops`), in declaration order, the order the framework starts and closes them. Their
/// names are unique.
template <auto... Handles>
	requires(isLoopHandle<std::remove_cvref_t<decltype(Handles)>> && ...)
struct Loops {
	static constexpr std::size_t count = sizeof...(Handles);
	static_assert(
		[] {
			constexpr std::array<std::string_view, count> names{decltype(Handles)::name...};
			for (std::size_t at = 0; at < count; ++at)
				for (std::size_t earlier = 0; earlier < at; ++earlier)
					if (names[at] == names[earlier]) return false;
			return true;
		}(),
		"a type's loops have unique names");
	/// Whether `Handle` is one of these loops.
	template <class Handle> static constexpr bool has =
		(std::same_as<Handle, std::remove_cvref_t<decltype(Handles)>> || ...);
	/// Where `Handle` sits among these loops; only for a loop `has` holds for.
	template <class Handle> static constexpr std::size_t indexOf = [] {
		constexpr std::array<bool, count> same{std::same_as<Handle, std::remove_cvref_t<decltype(Handles)>>...};
		return static_cast<std::size_t>(std::ranges::find(same, true) - same.begin());
	}();
	static constexpr std::array<LoopDeclaration, count> declared{LoopDeclaration{
		decltype(Handles)::name, decltype(Handles)::kind, std::remove_cvref_t<decltype(Handles)>::stateful}...};
};
template <class T> inline constexpr bool isLoops = false;
template <auto... Handles> inline constexpr bool isLoops<Loops<Handles...>> = true;

/// A loop as a type sees it (cx.executor(handle)), and as the framework's adapter implements it. Every handler runs on
/// the loop's thread, one at a time per loop, never inside the call that posted or armed it. A timer or a watch (the
/// Timer the arming calls return) is cancelled by its destruction: its handler never runs after the destructor returns.
/// It is destroyed on its loop's thread, or on the main loop while the node is not yet configured, or on any thread
/// once its loop closed (teardown step 4). Its state destroys every source it armed, before or in its destructor
/// (docs/writing-a-node-type.md#loops: one left armed is cancelled at the close and said, ear.loop-source-left).
class Loop : public Executor {
public:
	// Executor::post: any thread; serial, in post order; never runs `work` inside the call. The node's own intake:
	// accepted until its configuration's teardown step 4; work posted after that is destroyed unrun.
	/// `due` once, `delay` from now (CLOCK_MONOTONIC).
	[[nodiscard]] virtual std::unique_ptr<Timer> after(std::chrono::milliseconds delay,
	                                                   std::move_only_function<void()> due) = 0;
	/// `due` every `period` (at least 1 ms), at a fixed rate from now: ticks missed while a handler ran coalesce into one.
	[[nodiscard]] virtual std::unique_ptr<Timer> every(std::chrono::milliseconds period,
	                                                   std::move_only_function<void()> due) = 0;
	/// `rung` each time `wake` is rung: the loop answers the wake (Wake::answer, draining then clearing) just before
	/// it, so a ring during the handler runs it again. A wake rung before it was watched runs it once, on the loop's
	/// next turn. One watcher per wake; `wake` outlives the watch.
	[[nodiscard]] virtual std::unique_ptr<Timer> onWake(Wake& wake, std::move_only_function<void()> rung) = 0;
	/// `readable` each time `fd` is readable. The descriptor is the caller's and outlives the watch.
	[[nodiscard]] virtual std::unique_ptr<Timer> onReadable(int fd, std::move_only_function<void()> readable) = 0;
	virtual LoopKind kind() const noexcept = 0;
	/// Whether the calling thread is running one of this loop's handlers.
	virtual bool inLoop() const noexcept = 0;

protected:
	~Loop() = default;
};

/// What a loop's adapter tells NodeHost about one loop, on the main thread (ReadyEvents: the loop runs, or cannot
/// start).
class LoopEvents : public ReadyEvents {
public:
	/// A strand ran one handler for `ms`, past loopSlowBound: ear.loop-slow, once per loop and configuration.
	virtual void slow(std::int64_t ms) = 0;
	/// A handler threw `what` (the loop caught it, so nothing a type throws ends the process): the node fails as a hook
	/// that throws does (docs/writing-a-node-type.md#rules-the-framework-enforces). The loop runs on.
	virtual void threw(std::string what) = 0;

protected:
	~LoopEvents() = default;
};

/// A loop as the framework drives it, made by its adapter. Its handlers' work arrives through Loop (the node's own,
/// staged by NodeHost until the node is configured) and through postFromOutside (slice 5's actions).
class NodeLoop : public Loop {
public:
	/// Main thread, at teardown step 5, once closed: frees what is left (a dedicated loop's thread joined: it already
	/// returned).
	virtual ~NodeLoop() = default;
	/// Main thread. Work from outside the node: queued as the node's own is while the loop is open to others, else
	/// destroyed unrun at once.
	virtual void postFromOutside(std::move_only_function<void()> work) = 0;
	/// Main thread, at teardown step 1: outside work is refused from now on, and outside work still queued is destroyed
	/// unrun on the loop's thread. The node's own work and sources go on; a handler running finishes.
	virtual void closeToOthers() = 0;
	/// Main thread, at teardown step 4: runs the work already queued, then `last` (which destroys the loop's state, on
	/// its thread), then `finished` on the main thread. Work posted meanwhile, and after, is still taken (another loop's
	/// state may post a goodbye to this one as it goes) and runs after `last`.
	virtual void finish(std::move_only_function<void()> last, std::move_only_function<void()> finished) = 0;
	/// Main thread, once every loop of the configuration finished: takes no more work (what is posted later is
	/// destroyed unrun on the posting thread), runs the work already queued, cancels every source still armed (so no
	/// handler of one runs again), then `closed` on the main thread.
	virtual void close(std::move_only_function<void()> closed) = 0;
};

/// The loops' adapter (ResourceAdapters::loops), ear_runtime's Loops: strands on one pool, a dedicated loop on a thread
/// of its own.
class LoopAdapter {
public:
	/// Main thread. Loop `name` of `node`, of `kind`. It runs once `events.ready()` is told, never inside this call;
	/// `events.failed(why)` instead when it cannot start (no thread: the errno's text). `events` outlives the loop.
	virtual std::unique_ptr<NodeLoop> make(const NodeName& node, std::string_view name, LoopKind kind,
	                                       LoopEvents& events) = 0;

protected:
	~LoopAdapter() = default;
};

/// A strand handler that ran past this is ear.loop-slow (docs/output-records.md#diagnostic).
inline constexpr std::chrono::milliseconds loopSlowBound{100};

/// The name of a dedicated loop's thread: "ear-<name> <node>", cut to 15 bytes (what a thread's name holds).
std::string loopThreadName(std::string_view name, const NodeName& node);

/// Whether the calling thread is running a strand's handler, where nothing may block: SourceWriter's write and idle
/// assert it is not (docs/writing-a-node-type.md#loops). A strand's runner marks its handlers with StrandScope.
bool onStrand() noexcept;
class StrandScope {
public:
	StrandScope() noexcept;
	~StrandScope();
	StrandScope(const StrandScope&) = delete;
	StrandScope& operator=(const StrandScope&) = delete;

private:
	bool was_;
};

} // namespace ear
