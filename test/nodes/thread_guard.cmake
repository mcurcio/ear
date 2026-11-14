# cmake -DROOT=<folder> [-DGLOB=<pattern>] -P thread_guard.cmake
#
# The guard of docs/writing-a-node-type.md#loops: a node type's own code starts no thread; its work runs on its loops.
# Every file under ROOT matching GLOB (default: C and C++ sources and headers), but those with a `tests` folder in their
# path below ROOT (a type's tests may start threads), is read a line at a time, and a line naming std::thread,
# std::jthread or std::async, or calling pthread_create or fork, is printed with its place, and the script fails, as it
# does when ROOT and GLOB match no file at all (a guard over nothing passes nothing). It is a guard, not the
# enforcement: a library a type links (sendspin-cpp) starts threads of its own, which a grep cannot see.
if(NOT DEFINED ROOT)
	message(FATAL_ERROR "thread_guard: ROOT is not set")
endif()
if(NOT DEFINED GLOB)
	set(GLOB "*.cpp;*.hpp;*.c;*.h")
endif()

set(rules
	"std::(thread|jthread|async)([^A-Za-z0-9_]|$)"
	"(^|[^A-Za-z0-9_])pthread_create([^A-Za-z0-9_]|$)"
	"(^|[^A-Za-z0-9_])fork[ \t]*\\(")

set(patterns)
foreach(pattern IN LISTS GLOB)
	list(APPEND patterns "${ROOT}/${pattern}")
endforeach()
file(GLOB_RECURSE found LIST_DIRECTORIES false ${patterns})
set(files)
foreach(file IN LISTS found)
	file(RELATIVE_PATH below "${ROOT}" "${file}")
	if(NOT "/${below}" MATCHES "/tests/")
		list(APPEND files "${file}")
	endif()
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
				message(STATUS "${shown}:${number}: starts a thread: ${line}")
				math(EXPR offences "${offences} + 1")
				break()
			endif()
		endforeach()
	endforeach()
endforeach()

list(LENGTH files count)
if(count EQUAL 0)
	message(FATAL_ERROR "thread_guard: no file under ${ROOT} matches ${GLOB} outside tests: the guard would check nothing")
endif()
if(offences GREATER 0)
	message(FATAL_ERROR "thread_guard: ${offences} lines under ${ROOT} start a thread; "
		"a node's own work runs on its loops (docs/writing-a-node-type.md#loops)")
endif()
message(STATUS "thread_guard: ${count} files under ${ROOT}, none starts a thread")
