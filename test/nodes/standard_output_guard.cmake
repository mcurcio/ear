# cmake -DROOT=<folder> [-DGLOB=<pattern>] -P standard_output_guard.cmake
#
# The guard of docs/design/node-framework.md section 8.8: a node type's own code never writes standard output or
# standard error. Every file under ROOT matching GLOB (default: C and C++ sources and headers) is read a line at a time,
# and a line naming std::cout, std::cerr, std::clog, stdout, stderr, STDOUT_FILENO or STDERR_FILENO, calling printf,
# vprintf, puts, putchar or perror, C++23's print, println, vprint_unicode or vprint_nonunicode (which write standard
# output without naming it), or writing to descriptor 1 or 2 (write, dprintf) is printed with its place, and the script
# fails, as it does when ROOT and GLOB match no file at all (a guard over nothing passes nothing). It is a guard, not the enforcement: ear captures descriptors 1 and 2 at boot, which is what makes the
# rule true for code a grep cannot see (PipeWire, alsa-lib, a library).
if(NOT DEFINED ROOT)
	message(FATAL_ERROR "standard_output_guard: ROOT is not set")
endif()
if(NOT DEFINED GLOB)
	set(GLOB "*.cpp;*.hpp;*.c;*.h")
endif()

set(rules
	"std::(cout|cerr|clog)"
	"(^|[^A-Za-z0-9_])(stdout|stderr|STDOUT_FILENO|STDERR_FILENO)([^A-Za-z0-9_]|$)"
	"(^|[^A-Za-z0-9_])(printf|vprintf|puts|putchar|perror)[ \t]*\\("
	"(^|[^A-Za-z0-9_])(print|println|vprint_unicode|vprint_nonunicode)[ \t]*\\("
	"(^|[^A-Za-z0-9_])(write|dprintf)[ \t]*\\([ \t]*[12][ \t]*,")

set(patterns)
foreach(pattern IN LISTS GLOB)
	list(APPEND patterns "${ROOT}/${pattern}")
endforeach()
file(GLOB_RECURSE files LIST_DIRECTORIES false ${patterns})
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
				message(STATUS "${shown}:${number}: writes standard output or error: ${line}")
				math(EXPR offences "${offences} + 1")
				break()
			endif()
		endforeach()
	endforeach()
endforeach()

list(LENGTH files count)
if(count EQUAL 0)
	message(FATAL_ERROR "standard_output_guard: no file under ${ROOT} matches ${GLOB}: the guard would check nothing")
endif()
if(offences GREATER 0)
	message(FATAL_ERROR "standard_output_guard: ${offences} lines under ${ROOT} write standard output or error; "
		"a node's output goes through the framework (docs/design/node-framework.md section 8.8)")
endif()
message(STATUS "standard_output_guard: ${count} files under ${ROOT}, none writes standard output or error")
