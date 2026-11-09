# cmake -DROOT=<folder> [-DGLOB=<pattern>] -P callback_guard.cmake
#
# The guard of docs/architecture.md#the-callback-boundary: every C callback ear gives PipeWire is registered through
# the engine's callback boundary (src/pipewire/callback_boundary.hpp), whose functions take only guarded<> and
# realtime<> trampolines. Every file under ROOT matching GLOB (default: C and C++ sources and headers) is read a line at
# a time, except the boundary's own (a file named callback_boundary.*) and tests (anything under a folder named tests,
# which drive PipeWire themselves); a line naming one of PipeWire's registrations (pw_loop_add_*, pw_loop_invoke,
# pw_data_loop_invoke, spa_loop_invoke, spa_loop_utils_add_*, or calling any *_add_listener or *_add_object_listener)
# is printed with its place, and the script fails, as it does when ROOT and GLOB leave it no file to check (a guard over
# nothing passes nothing). The compile-time half is the boundary's types: its functions refuse a raw function pointer.
if(NOT DEFINED ROOT)
	message(FATAL_ERROR "callback_guard: ROOT is not set")
endif()
if(NOT DEFINED GLOB)
	set(GLOB "*.cpp;*.hpp;*.c;*.h")
endif()

set(rules
	"(^|[^A-Za-z0-9_])pw_loop_add_"
	"(^|[^A-Za-z0-9_])pw_loop_invoke([^A-Za-z0-9_]|$)"
	"(^|[^A-Za-z0-9_])pw_data_loop_invoke([^A-Za-z0-9_]|$)"
	"(^|[^A-Za-z0-9_])spa_loop_invoke([^A-Za-z0-9_]|$)"
	"(^|[^A-Za-z0-9_])spa_loop_utils_add_"
	"_add_listener[ \t]*\\("
	"_add_object_listener[ \t]*\\(")

set(patterns)
foreach(pattern IN LISTS GLOB)
	list(APPEND patterns "${ROOT}/${pattern}")
endforeach()
file(GLOB_RECURSE found LIST_DIRECTORIES false ${patterns})
set(files)
foreach(file IN LISTS found)
	file(RELATIVE_PATH shown "${ROOT}" "${file}")
	get_filename_component(name "${file}" NAME)
	if(name MATCHES "^callback_boundary\\." OR shown MATCHES "(^|/)tests/")
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
