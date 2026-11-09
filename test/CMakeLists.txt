# ear_catch_main: the Catch2 runner (vendored single header, main.cpp) as a library. Every test
# executable links it and gets Catch's header with it, then registers its test cases with
# ear_catch_tests(<target>) (one CTest test per Catch test case).
add_library(ear_catch_main STATIC main.cpp)
target_include_directories(ear_catch_main SYSTEM PUBLIC
	"${PROJECT_SOURCE_DIR}/vendor/catch/single_include")

# ear_catch_tests(<target>): catch_discover_tests with --warn NoTests, so a CTest test whose name
# matches no Catch test case fails (exit 2) instead of passing having run nothing. A Catch name
# holding ';' is split by CMake into two such tests, and Catch's own "No test cases matched"
# exits 0; this is what makes that, or any other filter that runs nothing, fail. Catch.cmake is
# included inside the function: it sets the path of its discovery script as a variable, which a
# sibling directory (src/<package>) would not see from here.
#
# SERIAL <test spec>: the test cases the Catch test spec matches (a tag, such as [s2]) are discovered apart, with the
# CTest property RUN_SERIAL, so ctest --parallel runs nothing beside them: for a test whose load would starve the
# realtime-timed tests running beside it (PipeWire's data thread keeping time), and theirs it.
function(ear_catch_tests target)
	cmake_parse_arguments(PARSE_ARGV 1 arg "" "SERIAL" "")
	if(arg_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "ear_catch_tests: unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
	endif()
	include(Catch)
	if(arg_SERIAL)
		catch_discover_tests(${target} TEST_SPEC "~${arg_SERIAL}" EXTRA_ARGS --warn NoTests)
		catch_discover_tests(${target} TEST_SPEC "${arg_SERIAL}" EXTRA_ARGS --warn NoTests PROPERTIES RUN_SERIAL TRUE)
	else()
		catch_discover_tests(${target} EXTRA_ARGS --warn NoTests)
	endif()
endfunction()

# ear_compile_check(<name> <source> [<flag>...]): compiles <source> (syntax only) against ear_core's headers as a test.
function(ear_compile_check name source)
	add_test(NAME ${name}
		COMMAND "${CMAKE_CXX_COMPILER}" ${CMAKE_CXX23_STANDARD_COMPILE_OPTION} -w -fsyntax-only
			"-I$<JOIN:$<TARGET_PROPERTY:ear_core,INTERFACE_INCLUDE_DIRECTORIES>,;-I>" ${ARGN} "${source}"
		COMMAND_EXPAND_LISTS)
endfunction()

# ear_compile_fail(<name> <source> <case> <regex>): <source> with EAR_CASE_<case> defined must be refused for the reason
# <regex> names. A case that compiles prints nothing and fails, and so does a wrong reason; a broken include is what
# the plain ear_compile_check of the same source catches. A '-' in <case> is '_' in the macro.
function(ear_compile_fail name source case regex)
	string(MAKE_C_IDENTIFIER "${case}" macro)
	ear_compile_check(${name} "${source}" -DEAR_CASE_${macro})
	set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

# The ear binary starts, loads its shared libraries and prints its options.
add_test(NAME ear_help COMMAND ear --help)
add_test(NAME ear_help_lists_config COMMAND ear --help)
# The graph file folder and the engine's fallback format, in the order --help lists them.
set_tests_properties(ear_help_lists_config PROPERTIES
	PASS_REGULAR_EXPRESSION "--config-dir.*--graph-frequency.*--block-size")

# What configure asks of a tree that must carry a patch (cmake/SendspinDeps.cmake).
add_test(NAME ear_carried_patch_check COMMAND "${CMAKE_COMMAND}"
	"-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/carried_patch_test" -P "${CMAKE_CURRENT_SOURCE_DIR}/carried_patch_test.cmake")

# The worked example of docs/writing-a-node-type.md, the `tone` type, taken from the page and compiled (syntax only)
# against today's headers, so the page cannot drift from them: the type, with the folder's name its table must carry,
# and its tests. It is never a node type of this build: nothing under src/nodes/ is made.
set(doc_tone "${CMAKE_CURRENT_BINARY_DIR}/doc_example/tone")
add_test(NAME ear_doc_example_extract COMMAND "${CMAKE_COMMAND}"
	"-DDOC=${PROJECT_SOURCE_DIR}/docs/writing-a-node-type.md" "-DOUT=${doc_tone}"
	-P "${CMAKE_CURRENT_SOURCE_DIR}/doc_example.cmake")
set_tests_properties(ear_doc_example_extract PROPERTIES FIXTURES_SETUP doc_example)
ear_compile_check(ear_doc_example_type "${doc_tone}/tone.cpp" "-I${doc_tone}" "-I${doc_tone}/include"
	"-DEAR_NODE_TYPE_NAME=\"tone\"")
ear_compile_check(ear_doc_example_tests "${doc_tone}/tests/tone_test.cpp" "-I${doc_tone}" "-I${doc_tone}/include"
	"-I${PROJECT_SOURCE_DIR}/vendor/catch/single_include")
set_tests_properties(ear_doc_example_type ear_doc_example_tests PROPERTIES FIXTURES_REQUIRED doc_example)

# ear_ws_client: the tests' WebSocket client (test/support/ws_client.hpp, header-only, on
# Boost.Beast), for the control port's tests (src/server/tests) and the system tests.
add_library(ear_ws_client INTERFACE)
target_include_directories(ear_ws_client INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(ear_ws_client INTERFACE Boost::beast)

# ear_temp_folder: the tests' temporary folder (test/support/temp_folder.hpp, header-only), for every test
# program that makes folders on disk.
add_library(ear_temp_folder INTERFACE)
target_include_directories(ear_temp_folder INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")

# ear_rtp_tone: the tests' RTP sender and tone fit (test/support/rtp_tone.hpp, header-only), for
# the RTP receiver's engine tests (src/pipewire/tests) and the system tests (docs/node-types.md#rtp).
add_library(ear_rtp_tone INTERFACE)
target_include_directories(ear_rtp_tone INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")

# ear_sendspin_server: the real Sendspin server for the sendspin node's clock and system tests (test/support/
# sendspin_server.py, run by sendspin_server.hpp, header-only, in the venv CMakeLists.txt checks for).
add_library(ear_sendspin_server INTERFACE)
target_include_directories(ear_sendspin_server INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_compile_definitions(ear_sendspin_server INTERFACE
	EAR_SENDSPIN_SERVER_PYTHON="${EAR_SENDSPIN_SERVER_PYTHON}"
	EAR_SENDSPIN_SERVER_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/support/sendspin_server.py")

# ear_node_types_tests: what holds for every node type together (each type's own tests are in src/nodes/<type>).
add_subdirectory(nodes)
