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
	# NodeTypeOf refuses, at compile time, a table not named for this folder.
	target_compile_definitions(${library} PRIVATE "EAR_NODE_TYPE_NAME=\"${type}\"")

	if(BUILD_TESTS)
		add_executable(${library}_tests ${arg_TESTS})
		# A type's tests run it through ear::testing::TypeHarness (docs/design/node-framework.md section 7.6), by its spec
		# and behaviour from the type's private header, src/nodes/<type>/<type>_spec.hpp.
		target_link_libraries(${library}_tests PRIVATE ${library} ear_type_harness ear_catch_main)
		target_include_directories(${library}_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
		add_dependencies(${library}_tests ${library}_verify_interface_header_sets)
		ear_catch_tests(${library}_tests)
	endif()

	set_property(GLOBAL APPEND PROPERTY EAR_NODE_TYPES ${type})
endfunction()

# ear_refuse_node_type_links(<type>...): stops configure, once the whole project has been read, if the library of one of
# these types reaches another's: directly, through a helper library, through an alias or from inside a generator
# expression, whichever spelling, wherever the link was made. It reads library targets only, so a test program may link
# what it needs. A generator expression is read for the targets it names, whatever its condition would come to.
function(ear_refuse_node_type_links)
	# Deferred calls expand their arguments when they run, in that directory, so the list is written into the call now.
	cmake_language(EVAL CODE
		"cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL ear_refuse_node_type_links_now ${ARGN})")
endfunction()

function(ear_refuse_node_type_links_now)
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
					# A link made from another directory carries that directory's id: name::@(id).
					string(REGEX REPLACE "::@.*$" "" entry "${entry}")
					set(named "${entry}")
					if(entry MATCHES "\\$<")
						string(REGEX MATCHALL "[A-Za-z0-9_.+-]+(::[A-Za-z0-9_.+-]+)*" named "${entry}")
					endif()
					foreach(name IN LISTS named)
						if(NOT TARGET "${name}")   # linked-NOTFOUND, a flag, a generator expression's own words
							continue()
						endif()
						get_target_property(aliased "${name}" ALIASED_TARGET)
						if(aliased)
							set(name "${aliased}")
						endif()
						if(name IN_LIST seen)
							continue()
						endif()
						if(name IN_LIST libraries)
							message(FATAL_ERROR "${start} links ${name} (through ${current}): no node type links another")
						endif()
						list(APPEND seen "${name}")
						list(APPEND waiting "${name}")
					endforeach()
				endforeach()
			endforeach()
		endwhile()
	endforeach()
endfunction()
