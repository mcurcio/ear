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
function(ear_catch_tests target)
	include(Catch)
	catch_discover_tests(${target} EXTRA_ARGS --warn NoTests)
endfunction()

# The ear binary starts, loads its shared libraries and prints its options.
add_test(NAME ear_help COMMAND ear --help)
add_test(NAME ear_help_lists_config COMMAND ear --help)
# The graph file folder and the engine's fallback format, in the order --help lists them.
set_tests_properties(ear_help_lists_config PROPERTIES
	PASS_REGULAR_EXPRESSION "--config-dir.*--graph-frequency.*--block-size")

# ear_ws_client: the tests' WebSocket client (test/support/ws_client.hpp, header-only, on
# Boost.Beast), for the control port's tests (src/server/tests) and the system tests.
add_library(ear_ws_client INTERFACE)
target_include_directories(ear_ws_client INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(ear_ws_client INTERFACE Boost::beast)

# ear_rtp_tone: the tests' RTP sender and tone fit (test/support/rtp_tone.hpp, header-only), for
# the RTP receiver's engine tests (src/pipewire/tests) and the system tests (docs/design/rtp.md).
add_library(ear_rtp_tone INTERFACE)
target_include_directories(ear_rtp_tone INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
