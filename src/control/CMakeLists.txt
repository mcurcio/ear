# ear_control: M1's control surface. It reads the graph file (ear.json v2), replays it as
# control calls and writes status lines (docs/design/m1-white-to-file.md section 1).
add_library(ear_control STATIC
	graph_file.cpp
	status_log.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_control PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/control/graph_file.hpp
	include/ear/control/status_log.hpp)
set_target_properties(ear_control PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_control PUBLIC ear_core)

if(BUILD_TESTS)
	include(Catch)
	add_executable(ear_control_tests
		tests/graph_file_test.cpp)
	target_link_libraries(ear_control_tests PRIVATE ear_control ear_catch_main)
	add_dependencies(ear_control_tests ear_control_verify_interface_header_sets)
	catch_discover_tests(ear_control_tests)
endif()
