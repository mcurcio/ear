// Forced into every source of ear_pipewire (-include, ear_poison_raw_callbacks in cmake/RawCallbacks.cmake): after the
// callback boundary (callback_boundary.hpp) every way to hand PipeWire or alsa-lib a C function to call is poisoned, so
// a raw registration outside the boundary does not compile (docs/architecture.md#the-callback-boundary). GCC refuses a
// poisoned identifier lexed after the pragma, however it is spelt ("attempt to use poisoned"), and leaves alone what
// was lexed before it: the boundary's templates and inline functions, and PipeWire's own.
//
// A PipeWire header read after the pragma that names a poisoned identifier fails to compile, so every PipeWire and SPA
// header ear_pipewire includes is included here first, and a source's own include of one is then a no-op. A source
// that needs a header missing here fails closed: add it below. The names are PipeWire 1.4.2's; a bump lists them again
// (the pipewire-engine skill, bumping PipeWire). Nothing here is used by a test program that drives PipeWire itself.
#pragma once

#include <dlfcn.h>

#include <pipewire/context.h>
#include <pipewire/data-loop.h>
#include <pipewire/filter.h>
#include <pipewire/impl-module.h>
#include <pipewire/impl-node.h>
#include <pipewire/impl.h>
#include <pipewire/log.h>
#include <pipewire/pipewire.h>
#include <spa/node/io.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>
#include <spa/support/log.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include "callback_boundary.hpp"

// Poisoning a defined macro warns, with no flag to silence it; the boundary's one use is already expanded.
#undef SPA_INTERFACE_INIT
#undef SPA_CALLBACKS_INIT

// Loop sources and hooks: a function the loop calls.
#pragma GCC poison pw_loop_add_io pw_loop_add_idle pw_loop_add_event pw_loop_add_timer pw_loop_add_signal
#pragma GCC poison pw_loop_add_hook spa_loop_utils_add_io spa_loop_utils_add_idle spa_loop_utils_add_event
#pragma GCC poison spa_loop_utils_add_timer spa_loop_utils_add_signal spa_loop_add_source spa_loop_control_add_hook
#pragma GCC poison spa_source
// Invokes and locked calls: a function run on a loop's thread.
#pragma GCC poison pw_loop_invoke pw_data_loop_invoke spa_loop_invoke pw_loop_locked spa_loop_locked pw_work_queue_add
// Listeners: an events table and a hook on an object's list.
#pragma GCC poison spa_hook spa_hook_list spa_hook_list_append spa_hook_list_prepend spa_hook_list_isolate
#pragma GCC poison spa_hook_remove pw_impl_node_remove_rt_listener
#pragma GCC poison pw_client_add_listener pw_client_endpoint_add_listener pw_client_node_add_listener
#pragma GCC poison pw_client_session_add_listener pw_context_add_listener pw_control_add_listener pw_core_add_listener
#pragma GCC poison pw_data_loop_add_listener pw_device_add_listener pw_endpoint_add_listener
#pragma GCC poison pw_endpoint_link_add_listener pw_endpoint_stream_add_listener pw_factory_add_listener
#pragma GCC poison pw_filter_add_listener pw_global_add_listener pw_impl_client_add_listener pw_impl_core_add_listener
#pragma GCC poison pw_impl_device_add_listener pw_impl_factory_add_listener pw_impl_link_add_listener
#pragma GCC poison pw_impl_metadata_add_listener pw_impl_module_add_listener pw_impl_node_add_listener
#pragma GCC poison pw_impl_node_add_rt_listener pw_impl_port_add_listener pw_link_add_listener
#pragma GCC poison pw_main_loop_add_listener pw_mempool_add_listener pw_metadata_add_listener pw_module_add_listener
#pragma GCC poison pw_node_add_listener pw_port_add_listener pw_profiler_add_listener pw_protocol_add_listener
#pragma GCC poison pw_proxy_add_listener pw_proxy_add_object_listener pw_registry_add_listener pw_resource_add_listener
#pragma GCC poison pw_resource_add_object_listener pw_security_context_add_listener pw_session_add_listener
#pragma GCC poison pw_stream_add_listener pw_thread_loop_add_listener spa_audio_aec_add_listener
#pragma GCC poison spa_dbus_connection_add_listener spa_device_add_listener spa_filter_graph_add_listener
#pragma GCC poison spa_node_add_listener
// Streams and filters made with their events.
#pragma GCC poison pw_stream_new_simple pw_filter_new_simple
// Interfaces and callbacks tables, made or handed over.
#pragma GCC poison SPA_INTERFACE_INIT SPA_CALLBACKS_INIT spa_interface spa_callbacks
#pragma GCC poison pw_client_endpoint_methods pw_client_methods pw_client_node_methods pw_client_session_methods
#pragma GCC poison pw_core_methods pw_device_methods pw_endpoint_link_methods pw_endpoint_methods
#pragma GCC poison pw_endpoint_stream_methods pw_factory_methods pw_link_methods pw_metadata_methods pw_module_methods
#pragma GCC poison pw_node_methods pw_port_methods pw_profiler_methods pw_registry_methods pw_security_context_methods
#pragma GCC poison pw_session_methods spa_audio_aec_methods spa_cpu_methods spa_dbus_methods spa_device_methods
#pragma GCC poison spa_filter_graph_methods spa_i18n_methods spa_log_methods spa_loop_control_methods spa_loop_methods
#pragma GCC poison spa_loop_utils_methods spa_node_methods spa_plugin_loader_methods spa_system_methods
#pragma GCC poison spa_thread_utils_methods
#pragma GCC poison pw_log_set pw_impl_node_set_implementation pw_impl_device_set_implementation
#pragma GCC poison pw_impl_factory_set_implementation pw_impl_metadata_set_implementation spa_node_set_callbacks
#pragma GCC poison spa_graph_node_set_callbacks spa_pod_builder_set_callbacks pw_global_new pw_thread_utils_create
#pragma GCC poison spa_thread_utils_create
// Walks that call a function back on the calling thread.
#pragma GCC poison pw_context_for_each_global pw_global_for_each_resource pw_impl_node_for_each_port
#pragma GCC poison pw_impl_node_for_each_param pw_impl_device_for_each_param pw_map_for_each pw_conf_section_for_each
#pragma GCC poison pw_context_conf_section_for_each pw_conf_match_rules pw_conf_section_match_rules
#pragma GCC poison pw_context_conf_section_match_rules
// By name: alsa-lib's handler, and a library function looked up at run time.
#pragma GCC poison snd_lib_error_set_handler dlsym dlvsym
