# Run by the tests ear_node_type_link_refused and ear_node_type_link_allowed (test/nodes/CMakeLists.txt): two node types
# and a helper library between them, linked only when EAR_LINK_OTHER is on. No languages, so it configures in moments.
cmake_minimum_required(VERSION 3.28...4.2)
project(LinkRefusal LANGUAGES NONE)
include("${EAR_CMAKE_DIR}/NodeType.cmake")

add_library(ear_node_a INTERFACE)
add_library(ear_helper INTERFACE)
add_library(ear_node_c INTERFACE)
if(EAR_LINK_OTHER)
	target_link_libraries(ear_node_a INTERFACE ear_helper)
	target_link_libraries(ear_helper INTERFACE ear_node_c)
endif()
ear_refuse_node_type_links(a c)
