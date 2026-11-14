# Sleeps by role (docs/testing.md#sleeps-by-role): a header forced into every source of every target that compiles test
# code poisons each raw sleep, and Catch's macros that tell and go on, after the role calls a test sleeps through.

# ear_poison_raw_sleeps(<target> <header> <roles>): every source of <target> is compiled with <header> forced in first,
# and <target> links <roles>, the interface library of the role calls.
function(ear_poison_raw_sleeps target header roles)
	target_compile_options(${target} PRIVATE "SHELL:-include ${header}")
	target_link_libraries(${target} PRIVATE ${roles})
endfunction()

# ear_refuse_raw_sleeps(<header> [EXEMPT <target>...]): stops configure, once the whole project has been read, at any
# target that compiles test code and is not compiled with <header> forced in by ear_poison_raw_sleeps, unless EXEMPT
# names it. A target compiles test code when one of its sources lies in the source tree, outside the build tree, in a
# folder named test, tests or testing. Interface libraries compile nothing and are not checked.
function(ear_refuse_raw_sleeps header)
	cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "EXEMPT")
	# Deferred calls expand their arguments when they run, in that directory, so the values are written into the call now.
	cmake_language(EVAL CODE
		"cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL ear_refuse_raw_sleeps_now [[${header}]] ${arg_EXEMPT})")
endfunction()

function(ear_refuse_raw_sleeps_now header)
	set(exempt ${ARGN})
	set(option "SHELL:-include ${header}")

	set(directories "")
	set(waiting "${CMAKE_SOURCE_DIR}")
	while(waiting)
		list(POP_FRONT waiting directory)
		list(APPEND directories "${directory}")
		get_property(below DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
		list(APPEND waiting ${below})
	endwhile()

	foreach(directory IN LISTS directories)
		get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
		foreach(target IN LISTS targets)
			get_target_property(type ${target} TYPE)
			if(type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY" OR target IN_LIST exempt)
				continue()
			endif()
			get_target_property(sources ${target} SOURCES)
			get_target_property(source_dir ${target} SOURCE_DIR)
			set(test_source "")
			foreach(source IN LISTS sources)
				cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${source_dir}" NORMALIZE OUTPUT_VARIABLE path)
				cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${path}" NORMALIZE in_build)
				cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${path}" NORMALIZE in_source)
				if(in_build OR NOT in_source)
					continue()
				endif()
				file(RELATIVE_PATH relative "${CMAKE_SOURCE_DIR}" "${path}")
				if(relative MATCHES "(^|/)(test|tests|testing)/")
					set(test_source "${relative}")
					break()
				endif()
			endforeach()
			if(NOT test_source)
				continue()
			endif()
			get_target_property(options ${target} COMPILE_OPTIONS)
			if(NOT option IN_LIST options)
				message(FATAL_ERROR "${target} compiles test code (${test_source}) without ${header} forced in: give it "
					"ear_poison_raw_sleeps, so a raw sleep does not compile (docs/testing.md#sleeps-by-role)")
			endif()
		endforeach()
	endforeach()
endfunction()
