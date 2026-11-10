// What must not compile about the callback boundary (src/pipewire/CMakeLists.txt registers one CTest test per case,
// each with the reason it must be refused; docs/architecture.md#the-callback-boundary). With no case defined this is
// the control, and it must compile: what the cases break is only what they name. Syntax only: nothing here runs.
#include <cstdint>

#include "callback_boundary.hpp"

namespace callback_boundary_bad {

using namespace ear::pipewire;

struct Owner {
	void onEvent(std::uint64_t /*count*/) {}
	void onTimeout(std::uint64_t /*expirations*/) {}
	void onState(pw_filter_state /*old*/, pw_filter_state /*state*/, const char* /*error*/) {}
	void onProcess(spa_io_position* /*position*/) noexcept {}
	void onProcessThrowing(spa_io_position* /*position*/) {}
	int onInvoke() noexcept { return 0; }
	void onWrongTimer(int /*signal*/) {}
	void onInvokeTimer(std::uint64_t /*expirations*/) noexcept {}
	void onStateNoexcept(pw_filter_state /*old*/, pw_filter_state /*state*/, const char* /*error*/) noexcept {}
	static void raw(void* /*data*/, std::uint64_t /*count*/) {}
	static int rawInvoke(spa_loop* /*loop*/, bool /*async*/, std::uint32_t /*seq*/, const void* /*data*/,
	                     std::size_t /*size*/, void* /*user*/) {
		return 0;
	}
	static void rawState(void* /*data*/, pw_filter_state /*old*/, pw_filter_state /*state*/, const char* /*error*/) {}
};
void logNoexcept(void* /*object*/, spa_log_level /*level*/, const char* /*file*/, int /*line*/, const char* /*func*/,
                 const char* /*format*/, ...) noexcept {}
void logThrowing(void* /*object*/, spa_log_level /*level*/, const char* /*file*/, int /*line*/, const char* /*func*/,
                 const char* /*format*/, ...) {}

struct Other {
	void onEvent(std::uint64_t /*count*/) {}
};

[[maybe_unused]] void registrations(pw_loop* loop, pw_data_loop* data, pw_filter* filter, Listener& hook,
                                    LoopSource& source, Owner& owner) {
	// The control: each registration as ear makes it. The data loop takes only realtime<>, every other loop guarded<>.
	addEvent(source, loop, guarded<&Owner::onEvent>, &owner);
	addTimer(source, loop, guarded<&Owner::onTimeout>, &owner);
	invoke(DataLoop(data), realtime<&Owner::onInvoke>, &owner, false);
	invoke(DataLoop::of(filter), realtime<&Owner::onInvoke>, &owner, false);
	invoke(loop, guarded<&Owner::onInvoke>, &owner, true);
	installLog<LogCallbacks{.log = realtimeFunction<&logNoexcept>}>(SPA_LOG_LEVEL_WARN);
	listen<FilterCallbacks<Owner>{.stateChanged = guarded<&Owner::onState>, .process = realtime<&Owner::onProcess>}>(
		filter, hook, &owner);

#ifdef EAR_CASE_raw_event
	// A raw function pointer is no Guarded: it has no catch.
	addEvent(source, loop, &Owner::raw, &owner);
#endif
#ifdef EAR_CASE_raw_invoke
	invoke(loop, &Owner::rawInvoke, &owner, false);
#endif
#ifdef EAR_CASE_raw_listener_entry
	// An events table's entry is made only from guarded<> (or, for the data loop, realtime<>).
	listen<FilterCallbacks<Owner>{.stateChanged = &Owner::rawState}>(filter, hook, &owner);
#endif
#ifdef EAR_CASE_guarded_process
	// process runs on the data loop: realtime<> only, never a catching trampoline.
	listen<FilterCallbacks<Owner>{.process = guarded<&Owner::onProcess>}>(filter, hook, &owner);
#endif
#ifdef EAR_CASE_realtime_not_noexcept
	listen<FilterCallbacks<Owner>{.process = realtime<&Owner::onProcessThrowing>}>(filter, hook, &owner);
#endif
#ifdef EAR_CASE_other_class
	// The object handed with a callback is the member's class.
	Other other;
	addEvent(source, loop, guarded<&Owner::onEvent>, &other);
#endif
#ifdef EAR_CASE_guarded_invoke_on_data_loop
	// The data loop never runs a catching trampoline.
	invoke(DataLoop(data), guarded<&Owner::onInvoke>, &owner, false);
#endif
#ifdef EAR_CASE_realtime_invoke_on_main_loop
	// A loop that is not the data loop runs only guarded<>.
	invoke(loop, realtime<&Owner::onInvoke>, &owner, false);
#endif
#ifdef EAR_CASE_realtime_timer
	// A timer, event or signal source runs on a loop that may run anything: guarded<> only.
	addTimer(source, loop, realtime<&Owner::onInvokeTimer>, &owner);
#endif
#ifdef EAR_CASE_realtime_in_guarded_entry
	listen<FilterCallbacks<Owner>{.stateChanged = realtime<&Owner::onStateNoexcept>}>(filter, hook, &owner);
#endif
#ifdef EAR_CASE_raw_log_method
	// A log method is a realtime function, never a raw pointer.
	installLog<LogCallbacks{.log = &logNoexcept}>(SPA_LOG_LEVEL_WARN);
#endif
#ifdef EAR_CASE_log_method_not_noexcept
	installLog<LogCallbacks{.log = realtimeFunction<&logThrowing>}>(SPA_LOG_LEVEL_WARN);
#endif
#ifdef EAR_CASE_wrong_signature
	// A timer's member takes the expirations.
	addTimer(source, loop, guarded<&Owner::onWrongTimer>, &owner);
#endif
}

} // namespace callback_boundary_bad
