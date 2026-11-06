# ear_node_rtp: the "rtp" node type (docs/design/rtp.md). It reaches the engine only through ear_core's interfaces: an
# RTP receiver is asked for through the node's grant.
add_library(ear_node_rtp STATIC rtp.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_node_rtp PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/ear/nodes/rtp.hpp)
set_target_properties(ear_node_rtp PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_node_rtp PUBLIC ear_core)

if(BUILD_TESTS)
	add_executable(ear_node_rtp_tests tests/rtp_test.cpp)
	target_link_libraries(ear_node_rtp_tests PRIVATE ear_node_rtp ear_catch_main)
	add_dependencies(ear_node_rtp_tests ear_node_rtp_verify_interface_header_sets)
	ear_catch_tests(ear_node_rtp_tests)
endif()
