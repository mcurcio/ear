# What makes a folder in src/nodes/ a node type, and the rule that no type links another. Function definitions only, so
# a project with no languages can include this file (test/nodes/link_refusal does).

# ear_node_type(SOURCES <file>... TESTS <file>...), called from src/nodes/<type>/CMakeLists.txt. The folder names the
# type: the library ear_node_<type> with the checked header include/ear/nodes/<type>.hpp and, with BUILD_TESTS, the
# program ear_node_<type>_tests. Both are made here alone, so no type can leave out its tests or its header check; the
# type's own file carries on with plain CMake on those target names, for what else it links. TESTS is demanded with
# BUILD_TESTS off too, so a type cannot be right only in the build that does not look.
function(ear_node_type)
	cmake_parse_arguments(PARSE_ARGV 0 arg "" "" "SOURCES;TESTS")
	cmake_path(GET CMAKE_CURRENT_SOURCE_DIR FILENAME type)
	if(arg_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "ear_node_type() in src/nodes/${type}: unknown arguments: ${arg_UNPARSED_ARGUMENTS}")
	endif()
	# A folder's name is pasted into a header's path and a function's name (src/all_node_types), so it has one
	# capitalisation and is always a valid identifier.
	if(NOT type MATCHES "^[a-z][a-z0-9]*$")
		message(FATAL_ERROR "src/nodes/${type}: a node type's folder is a lower-case letter, then lower-case letters and digits")
	endif()
	if(NOT arg_SOURCES OR NOT arg_TESTS)
		message(FATAL_ERROR "ear_node_type() in src/nodes/${type} needs SOURCES and TESTS")
	endif()
	set(header "include/ear/nodes/${type}.hpp")
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${header}")
		message(FATAL_ERROR "src/nodes/${type} has no public header: ${header}")
	endif()

	set(library ear_node_${type})
	add_library(${library} STATIC ${arg_SOURCES})
	# Header set and its standalone compile check: see src/core/CMakeLists.txt.
	target_sources(${library} PUBLIC FILE_SET HEADERS BASE_DIRS include FILES "${header}")
	set_target_properties(${library} PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
	target_link_libraries(${library} PUBLIC ear_core)

	if(BUILD_TESTS)
		add_executable(${library}_tests ${arg_TESTS})
		target_link_libraries(${library}_tests PRIVATE ${library} ear_catch_main)
		add_dependencies(${library}_tests ${library}_verify_interface_header_sets)
		ear_catch_tests(${library}_tests)
	endif()

	set_property(GLOBAL APPEND PROPERTY EAR_NODE_TYPES ${type})
endfunction()

# ear_refuse_node_type_links(<type>...): stops configure if the library of one of these types reaches another's, directly
# or through a helper library. It reads library targets only, so a test program may link what it needs.
function(ear_refuse_node_type_links)
	set(libraries "")
	foreach(type IN LISTS ARGN)
		list(APPEND libraries ear_node_${type})
	endforeach()
	foreach(start IN LISTS libraries)
		set(seen ${start})
		set(waiting ${start})
		while(waiting)
			list(POP_FRONT waiting current)
			foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
				get_target_property(linked ${current} ${property})
				foreach(entry IN LISTS linked)
					string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" entry "${entry}")
					# A link made from another directory carries that directory's id: name::@(id).
					string(REGEX REPLACE "::@.*$" "" entry "${entry}")
					if(entry IN_LIST seen OR NOT TARGET "${entry}")   # linked-NOTFOUND, a flag, a generator expression
						continue()
					endif()
					if(entry IN_LIST libraries)
						message(FATAL_ERROR "${start} links ${entry} (through ${current}): no node type links another")
					endif()
					list(APPEND seen ${entry})
					list(APPEND waiting ${entry})
				endforeach()
			endforeach()
		endwhile()
	endforeach()
endfunction()
