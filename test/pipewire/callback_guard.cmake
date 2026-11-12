# cmake -DROOT=<folder> [-DGLOB=<pattern>] [-DEXEMPT=<paths>] -P callback_guard.cmake
#
# The guard of docs/architecture.md#the-callback-boundary: every C function ear gives PipeWire or alsa-lib to call is
# registered through the engine's callback boundary (src/pipewire/callback_boundary.hpp), whose functions take only
# guarded<>, realtime<> and realtimeFunction<> values. Every file under ROOT matching GLOB (default: C and C++ sources
# and headers) is read a line at a time, except the paths EXEMPT names relative to ROOT (default: the boundary's own two
# files, pipewire/callback_boundary.hpp and .cpp, exactly) and tests (anything under a folder named tests, which drive
# PipeWire themselves). A line naming a way to hand PipeWire a callback is printed with its place, and the script fails,
# as it does when ROOT and GLOB leave it no file to check (a guard over nothing passes nothing): a loop source, invoke
# or locked call (pw_loop_add_*, spa_loop_add_source, spa_loop_utils_add_*, pw_loop_invoke, pw_data_loop_invoke,
# spa_loop_invoke, pw_loop_locked, spa_loop_locked, pw_work_queue_add); a listener (any *_add_listener,
# *_add_object_listener or *_add_rt_listener, its parenthesis on this line or the next, or spa_hook_list_append); a stream or filter made
# with its events (pw_stream_new_simple, pw_filter_new_simple); an interface implemented (SPA_INTERFACE_INIT,
# SPA_CALLBACKS_INIT, spa_callbacks, a spa_*_methods or pw_*_methods table); PipeWire's log (pw_log_set); alsa-lib's
# error handler (snd_lib_error_set_handler). The compile-time half is the boundary's types: its functions refuse a raw
# function pointer.
if(NOT DEFINED ROOT)
	message(FATAL_ERROR "callback_guard: ROOT is not set")
endif()
if(NOT DEFINED GLOB)
	set(GLOB "*.cpp;*.hpp;*.c;*.h")
endif()
if(NOT DEFINED EXEMPT)
	set(EXEMPT "pipewire/callback_boundary.hpp;pipewire/callback_boundary.cpp")
endif()

# A name is whole: not part of a longer identifier on either side.
set(before "(^|[^A-Za-z0-9_])")
set(after "([^A-Za-z0-9_]|$)")
set(rules
	"${before}pw_loop_add_"
	"${before}spa_loop_add_source${after}"
	"${before}spa_loop_utils_add_"
	"${before}(pw_loop_invoke|pw_data_loop_invoke|spa_loop_invoke)${after}"
	"${before}(pw_loop_locked|spa_loop_locked|pw_work_queue_add)${after}"
	"_add_(object_|rt_)?listener${after}"
	"${before}spa_hook_list_append${after}"
	"${before}(pw_stream_new_simple|pw_filter_new_simple)${after}"
	"${before}(SPA_INTERFACE_INIT|SPA_CALLBACKS_INIT|spa_callbacks)${after}"
	"${before}(spa|pw)_[a-z_]+_methods${after}"
	"${before}pw_log_set${after}"
	"${before}snd_lib_error_set_handler${after}")

set(patterns)
foreach(pattern IN LISTS GLOB)
	list(APPEND patterns "${ROOT}/${pattern}")
endforeach()
file(GLOB_RECURSE found LIST_DIRECTORIES false ${patterns})
set(files)
foreach(file IN LISTS found)
	file(RELATIVE_PATH shown "${ROOT}" "${file}")
	list(FIND EXEMPT "${shown}" exempt) # a -P script runs under old policies: no IN_LIST
	if(NOT exempt EQUAL -1 OR shown MATCHES "(^|/)tests/")
		continue()
	endif()
	list(APPEND files "${file}")
endforeach()
list(SORT files)

set(offences 0)
foreach(file IN LISTS files)
	file(STRINGS "${file}" lines)
	set(number 0)
	foreach(line IN LISTS lines)
		math(EXPR number "${number} + 1")
		foreach(rule IN LISTS rules)
			if(line MATCHES "${rule}")
				file(RELATIVE_PATH shown "${ROOT}" "${file}")
				message(STATUS "${shown}:${number}: registers a callback outside the boundary: ${line}")
				math(EXPR offences "${offences} + 1")
				break()
			endif()
		endforeach()
	endforeach()
endforeach()

list(LENGTH files count)
if(count EQUAL 0)
	message(FATAL_ERROR "callback_guard: the guard would check nothing: no file under ${ROOT} matches ${GLOB} "
		"outside the boundary and tests")
endif()
if(offences GREATER 0)
	message(FATAL_ERROR "callback_guard: ${offences} lines under ${ROOT} register a callback with PipeWire themselves; "
		"register it through src/pipewire/callback_boundary.hpp (docs/architecture.md#the-callback-boundary)")
endif()
message(STATUS "callback_guard: ${count} files under ${ROOT}, none registers a callback outside the boundary")
