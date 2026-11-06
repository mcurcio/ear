# ear_node_types_tests: what holds for every node type together. It lists the types itself, as main.cpp does; each
# type's own tests are in its folder, src/nodes/<type>.
add_executable(ear_node_types_tests node_types_test.cpp)
target_link_libraries(ear_node_types_tests PRIVATE ear_node_card ear_node_file ear_node_rtp ear_node_sendspin
	ear_node_white ear_catch_main)
ear_catch_tests(ear_node_types_tests)
