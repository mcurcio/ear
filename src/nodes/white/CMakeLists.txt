# ear_node_white: the "white" node type, uniform noise on one output (docs/design/m1-white-to-file.md section 2).
add_library(ear_node_white STATIC white.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_node_white PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/ear/nodes/white.hpp)
set_target_properties(ear_node_white PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_node_white PUBLIC ear_core)

if(BUILD_TESTS)
	add_executable(ear_node_white_tests tests/white_test.cpp)
	target_link_libraries(ear_node_white_tests PRIVATE ear_node_white ear_catch_main)
	add_dependencies(ear_node_white_tests ear_node_white_verify_interface_header_sets)
	ear_catch_tests(ear_node_white_tests)
endif()
