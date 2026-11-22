# cmake -DROOT=<folder> [-DGLOB=<pattern>] -P erased_layer_guard.cmake
#
# The guard of docs/writing-a-node-type.md#configure-and-createof: a node type's own code answers its configure through
# its context (`cx.applied(...)`), which holds it to its spec, and never through the erased layer beneath, which checks
# nothing at compile time. Every file under ROOT matching GLOB (default: C and C++ sources and headers), but those with a
# `tests` folder in their path below ROOT (a type's tests may write an instance by hand), is read a line at a time, and
# a line naming ErasedInstance, ErasedLoopState, CreateContext, Configured::erased, NodeCode::passThrough, the tests'
# FIFO maker (fifoOn, FifoMaker) or a header of ear/testing/ is printed with its place, and the script fails, as it does
# when ROOT and GLOB match no file at all (a guard over nothing passes nothing). The compiler refuses most of these
# already (the erased answer is ErasedInstance's alone, a FIFO part's constructor private); the guard refuses the ways
# round it, a class derived to reach a protected member among them.
if(NOT DEFINED ROOT)
	message(FATAL_ERROR "erased_layer_guard: ROOT is not set")
endif()
if(NOT DEFINED GLOB)
	set(GLOB "*.cpp;*.hpp;*.c;*.h")
endif()

set(rules
	"(^|[^A-Za-z0-9_])(ErasedInstance|ErasedLoopState|CreateContext|FifoMaker|fifoOn)([^A-Za-z0-9_]|$)"
	"Configured::erased([^A-Za-z0-9_]|$)"
	"NodeCode::passThrough([^A-Za-z0-9_]|$)"
	"#[ \t]*include[ \t]*[<\"]ear/testing/")

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
				message(STATUS "${shown}:${number}: reaches the erased layer: ${line}")
				math(EXPR offences "${offences} + 1")
				break()
			endif()
		endforeach()
	endforeach()
endforeach()

list(LENGTH files count)
if(count EQUAL 0)
	message(FATAL_ERROR "erased_layer_guard: no file under ${ROOT} matches ${GLOB} outside tests: the guard would check nothing")
endif()
if(offences GREATER 0)
	message(FATAL_ERROR "erased_layer_guard: ${offences} lines under ${ROOT} reach the erased layer; a node's "
		"configure answers through its context (docs/writing-a-node-type.md#configure-and-createof)")
endif()
message(STATUS "erased_layer_guard: ${count} files under ${ROOT}, none reaches the erased layer")
