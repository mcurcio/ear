# ear_node_types_tests: what holds for every node type together. It links the generated list alone and names no type's
# library; each type's own tests are in its folder, src/nodes/<type>. test/ is added before src/, so the folder list is a
# generator expression, read when the build is generated.
add_executable(ear_node_types_tests node_types_test.cpp settings_refusals_test.cpp)
target_link_libraries(ear_node_types_tests PRIVATE ear_all_node_types ear_folder ear_temp_folder ear_catch_main)
target_compile_definitions(ear_node_types_tests PRIVATE
	"EAR_NODE_TYPE_FOLDERS=\"$<JOIN:$<TARGET_PROPERTY:ear_all_node_types,EAR_NODE_TYPES>,$<COMMA>>\"")
add_dependencies(ear_node_types_tests ear_all_node_types_verify_interface_header_sets)
ear_catch_tests(ear_node_types_tests)

# Configure stops when one node type links another, even through a helper, and goes on when none does: link_refusal is a
# project of its own, because the refusal stops the whole configure.
function(ear_node_type_link_test name link_other)
	add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/link_refusal"
		-B "${CMAKE_CURRENT_BINARY_DIR}/${name}" "-DEAR_CMAKE_DIR=${PROJECT_SOURCE_DIR}/cmake"
		-DEAR_LINK_OTHER=${link_other})
endfunction()
ear_node_type_link_test(ear_node_type_link_refused ON)
set_tests_properties(ear_node_type_link_refused PROPERTIES PASS_REGULAR_EXPRESSION "ear_node_a links ear_node_c")
ear_node_type_link_test(ear_node_type_link_allowed OFF)
