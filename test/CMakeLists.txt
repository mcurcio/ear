# ear_catch_main: the Catch2 runner (vendored single header, main.cpp) as a library. Every test
# executable links it and gets Catch's header with it, then registers its test cases with
# include(Catch) and catch_discover_tests(<target>) (one CTest test per Catch test case).
add_library(ear_catch_main STATIC main.cpp)
target_include_directories(ear_catch_main SYSTEM PUBLIC
	"${PROJECT_SOURCE_DIR}/vendor/catch/single_include")

# The ear binary starts, loads its shared libraries and prints its options.
add_test(NAME ear_help COMMAND ear --help)
add_test(NAME ear_help_lists_config COMMAND ear --help)
set_tests_properties(ear_help_lists_config PROPERTIES PASS_REGULAR_EXPRESSION "--config")

# ear_ws_client: the tests' WebSocket client (test/support/ws_client.hpp, header-only, on
# Boost.Beast), for the control port's tests (src/server/tests) and the system tests.
add_library(ear_ws_client INTERFACE)
target_include_directories(ear_ws_client INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(ear_ws_client INTERFACE Boost::beast)
