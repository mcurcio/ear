// What must not compile outside the callback boundary (src/pipewire/CMakeLists.txt registers one CTest test per case,
// each with the reason it must be refused; docs/architecture.md#the-callback-boundary). Compiled as ear_pipewire's
// sources are, with no_raw_callbacks.hpp forced in first. With no case defined this is the control, and it must
// compile: the boundary's registrations, made from its templates after the poison, and the PipeWire calls ear makes
// that hand over no callback. Syntax only: nothing here runs.
#include <cstdint>
#include <memory>

#include <pipewire/pipewire.h> // already read by the forced header: nothing new, so nothing poisoned

#include "callback_boundary.hpp"

namespace no_raw_callbacks_bad {

using namespace ear::pipewire;

struct Owner {
	void onEvent(std::uint64_t /*count*/) {}
	void onTimeout(std::uint64_t /*expirations*/) {}
	void onSignal(int /*signal*/) {}
	void onState(pw_filter_state /*old*/, pw_filter_state /*state*/, const char* /*error*/) {}
	void onProcess(spa_io_position* /*position*/) noexcept {}
	void onDone(std::uint32_t /*id*/, int /*seq*/) {}
	int onInvoke() noexcept { return 0; }
	void onStart() noexcept {}
	static void raw(void* /*data*/, std::uint64_t /*count*/) {}
};
void logNoexcept(void* /*object*/, spa_log_level /*level*/, const char* /*file*/, int /*line*/, const char* /*func*/,
                 const char* /*format*/, ...) noexcept {}
void alsaNoexcept(const char* /*file*/, int /*line*/, const char* /*function*/, int /*error*/, const char* /*format*/,
                  ...) noexcept {}

[[maybe_unused]] void registrations(pw_loop* loop, pw_data_loop* data, pw_filter* filter, pw_core* core,
                                    pw_context* context, Listener& hook, LoopSource& source, pw_impl_node* clock,
                                    RealtimeListener<Owner>& rtHook, Owner& owner) {
	// The control: each registration as ear makes it, through the boundary.
	addEvent(source, loop, guarded<&Owner::onEvent>, &owner);
	addTimer(source, loop, guarded<&Owner::onTimeout>, &owner);
	addSignal(source, loop, 15, guarded<&Owner::onSignal>, &owner);
	invoke(DataLoop(data), realtime<&Owner::onInvoke>, &owner, false);
	invoke(loop, guarded<&Owner::onInvoke>, &owner, true);
	listen<FilterCallbacks<Owner>{.stateChanged = guarded<&Owner::onState>, .process = realtime<&Owner::onProcess>}>(
		filter, hook, &owner);
	listen<CoreCallbacks<Owner>{.done = guarded<&Owner::onDone>}>(core, hook, &owner);
	listenRealtime<NodeRealtimeCallbacks<Owner>{.start = realtime<&Owner::onStart>}>(clock, rtHook,
	                                                                                 std::make_unique<Owner>());
	installLog<LogCallbacks{.log = realtimeFunction<&logNoexcept>}>(SPA_LOG_LEVEL_WARN);
	setAlsaErrorHandler(realtimeFunction<&alsaNoexcept>);
	[[maybe_unused]] pw_impl_node* found = engineNode(context, "ear.internal-clock");
	// And what ear calls that hands over no callback: waking a source, arming a timer, a thread loop.
	pw_loop_signal_event(loop, source.get());
	timespec value{};
	pw_loop_update_timer(loop, source.get(), &value, nullptr, false);
	pw_thread_loop_destroy(pw_thread_loop_new("control", nullptr));

#ifdef EAR_CASE_raw_call
	// A loop source registered by hand: its callback has no catch.
	pw_loop_add_event(loop, &Owner::raw, &owner);
#endif
#ifdef EAR_CASE_split_call
	// The call's parenthesis on the next line: a token is a token.
	// clang-format off
	pw_filter_add_listener
		(filter, nullptr, nullptr, &owner);
	// clang-format on
#endif
#ifdef EAR_CASE_pasted
	// A name made by pasting, in a macro of the source's own.
#define EAR_PASTE(a, b) a##b
	EAR_PASTE(pw_loop_, invoke)(loop, nullptr, 0, nullptr, 0, false, &owner);
#endif
#ifdef EAR_CASE_spliced
	// A name spliced across lines by a backslash-newline, joined before it is a token.
	// clang-format off
	pw_core_add_\
listener(core, nullptr, nullptr, &owner);
	// clang-format on
#endif
#ifdef EAR_CASE_macro_after
	// A macro of the source's own naming one: refused where it is defined.
#define EAR_REGISTER pw_impl_node_add_rt_listener
#endif
#ifdef EAR_CASE_hook_by_hand
	// A hook made by hand: every listener needs one, a new listener API on a PipeWire bump included.
	spa_hook raw{};
#endif
#ifdef EAR_CASE_interface_by_hand
	// An interface made by hand, its methods ours with no noexcept asked of them.
	static spa_log log{.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Log, SPA_VERSION_LOG, nullptr, nullptr)};
#endif
#ifdef EAR_CASE_walk
	// A walk PipeWire calls back on this thread: a throw from the lambda would unwind through C.
	pw_context_for_each_global(context, [](void* /*data*/, pw_global* /*global*/) { return 0; }, nullptr);
#endif
#ifdef EAR_CASE_by_name
	// Any of them looked up by name at run time.
	[[maybe_unused]] void* add = dlsym(RTLD_DEFAULT, "pw_loop_add_event");
#endif
}

} // namespace no_raw_callbacks_bad

#ifdef EAR_CASE_later_header
// A PipeWire header the forced one does not read, naming a poisoned identifier: it fails closed until it is added there.
#include <pipewire/extensions/profiler.h>
#endif

#ifdef EAR_CASE_every_name
// Every identifier the forced header poisons, one per line: compile_fail/every_name.cmake fails naming each line the
// compiler does not refuse as poisoned. A name added to the header is added here.
// clang-format off
pw_loop_add_io
pw_loop_add_idle
pw_loop_add_event
pw_loop_add_timer
pw_loop_add_signal
pw_loop_add_hook
spa_loop_utils_add_io
spa_loop_utils_add_idle
spa_loop_utils_add_event
spa_loop_utils_add_timer
spa_loop_utils_add_signal
spa_loop_add_source
spa_loop_control_add_hook
spa_source
pw_loop_invoke
pw_data_loop_invoke
spa_loop_invoke
pw_loop_locked
spa_loop_locked
pw_work_queue_add
spa_hook
spa_hook_list
spa_hook_list_append
spa_hook_list_prepend
spa_hook_list_isolate
spa_hook_remove
pw_impl_node_remove_rt_listener
pw_client_add_listener
pw_client_endpoint_add_listener
pw_client_node_add_listener
pw_client_session_add_listener
pw_context_add_listener
pw_control_add_listener
pw_core_add_listener
pw_data_loop_add_listener
pw_device_add_listener
pw_endpoint_add_listener
pw_endpoint_link_add_listener
pw_endpoint_stream_add_listener
pw_factory_add_listener
pw_filter_add_listener
pw_global_add_listener
pw_impl_client_add_listener
pw_impl_core_add_listener
pw_impl_device_add_listener
pw_impl_factory_add_listener
pw_impl_link_add_listener
pw_impl_metadata_add_listener
pw_impl_module_add_listener
pw_impl_node_add_listener
pw_impl_node_add_rt_listener
pw_impl_port_add_listener
pw_link_add_listener
pw_main_loop_add_listener
pw_mempool_add_listener
pw_metadata_add_listener
pw_module_add_listener
pw_node_add_listener
pw_port_add_listener
pw_profiler_add_listener
pw_protocol_add_listener
pw_proxy_add_listener
pw_proxy_add_object_listener
pw_registry_add_listener
pw_resource_add_listener
pw_resource_add_object_listener
pw_security_context_add_listener
pw_session_add_listener
pw_stream_add_listener
pw_thread_loop_add_listener
spa_audio_aec_add_listener
spa_dbus_connection_add_listener
spa_device_add_listener
spa_filter_graph_add_listener
spa_node_add_listener
pw_stream_new_simple
pw_filter_new_simple
SPA_INTERFACE_INIT
SPA_CALLBACKS_INIT
spa_interface
spa_callbacks
pw_client_endpoint_methods
pw_client_methods
pw_client_node_methods
pw_client_session_methods
pw_core_methods
pw_device_methods
pw_endpoint_link_methods
pw_endpoint_methods
pw_endpoint_stream_methods
pw_factory_methods
pw_link_methods
pw_metadata_methods
pw_module_methods
pw_node_methods
pw_port_methods
pw_profiler_methods
pw_registry_methods
pw_security_context_methods
pw_session_methods
spa_audio_aec_methods
spa_cpu_methods
spa_dbus_methods
spa_device_methods
spa_filter_graph_methods
spa_i18n_methods
spa_log_methods
spa_loop_control_methods
spa_loop_methods
spa_loop_utils_methods
spa_node_methods
spa_plugin_loader_methods
spa_system_methods
spa_thread_utils_methods
pw_log_set
pw_impl_node_set_implementation
pw_impl_device_set_implementation
pw_impl_factory_set_implementation
pw_impl_metadata_set_implementation
spa_node_set_callbacks
spa_graph_node_set_callbacks
spa_pod_builder_set_callbacks
pw_global_new
pw_thread_utils_create
spa_thread_utils_create
pw_context_for_each_global
pw_global_for_each_resource
pw_impl_node_for_each_port
pw_impl_node_for_each_param
pw_impl_device_for_each_param
pw_map_for_each
pw_conf_section_for_each
pw_context_conf_section_for_each
pw_conf_match_rules
pw_conf_section_match_rules
pw_context_conf_section_match_rules
snd_lib_error_set_handler
dlsym
dlvsym
// clang-format on
#endif
