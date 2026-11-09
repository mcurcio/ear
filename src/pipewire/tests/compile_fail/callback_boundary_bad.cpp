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
	static void raw(void* /*data*/, std::uint64_t /*count*/) {}
	static int rawInvoke(spa_loop* /*loop*/, bool /*async*/, std::uint32_t /*seq*/, const void* /*data*/,
	                     std::size_t /*size*/, void* /*user*/) {
		return 0;
	}
	static void rawState(void* /*data*/, pw_filter_state /*old*/, pw_filter_state /*state*/, const char* /*error*/) {}
};
struct Other {
	void onEvent(std::uint64_t /*count*/) {}
};

[[maybe_unused]] void registrations(pw_loop* loop, pw_filter* filter, spa_hook& hook, Owner& owner) {
	// The control: each registration as ear makes it.
	addEvent(loop, guarded<&Owner::onEvent>, &owner);
	addTimer(loop, guarded<&Owner::onTimeout>, &owner);
	invoke(loop, realtime<&Owner::onInvoke>, &owner, false);
	invoke(loop, guarded<&Owner::onInvoke>, &owner, true);
	listen<FilterCallbacks<Owner>{.stateChanged = guarded<&Owner::onState>, .process = realtime<&Owner::onProcess>}>(
		filter, hook, &owner);

#ifdef EAR_CASE_raw_event
	// A raw function pointer is no Guarded: it has no catch.
	addEvent(loop, &Owner::raw, &owner);
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
	addEvent(loop, guarded<&Owner::onEvent>, &other);
#endif
#ifdef EAR_CASE_wrong_signature
	// A timer's member takes the expirations.
	addTimer(loop, guarded<&Owner::onWrongTimer>, &owner);
#endif
}

} // namespace callback_boundary_bad
