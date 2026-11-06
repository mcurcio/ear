# ear_node_card: the "card" node type (docs/design/batch2.md section 4.4). It reaches the engine only through ear_core's
# interfaces: a sound card is asked for through the node's grant.
add_library(ear_node_card STATIC card.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_node_card PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/ear/nodes/card.hpp)
set_target_properties(ear_node_card PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_node_card PUBLIC ear_core)

if(BUILD_TESTS)
	add_executable(ear_node_card_tests tests/card_test.cpp)
	target_link_libraries(ear_node_card_tests PRIVATE ear_node_card ear_catch_main)
	add_dependencies(ear_node_card_tests ear_node_card_verify_interface_header_sets)
	ear_catch_tests(ear_node_card_tests)
endif()
