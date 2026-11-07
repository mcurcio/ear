# ear_node_types_tests: what holds for every node type together. It links the generated list alone and names no type's
# library; each type's own tests are in its folder, src/nodes/<type>. test/ is added before src/, so the folder list is a
# generator expression, read when the build is generated.
add_executable(ear_node_types_tests harness_test.cpp node_types_test.cpp settings_refusals_test.cpp)
target_link_libraries(ear_node_types_tests PRIVATE
	ear_all_node_types ear_type_harness ear_folder ear_temp_folder ear_catch_main)
target_compile_definitions(ear_node_types_tests PRIVATE
	"EAR_NODE_TYPE_FOLDERS=\"$<JOIN:$<TARGET_PROPERTY:ear_all_node_types,EAR_NODE_TYPES>,$<COMMA>>\"")
add_dependencies(ear_node_types_tests ear_all_node_types_verify_interface_header_sets)
ear_catch_tests(ear_node_types_tests)

# Configure stops when one node type links another, however it is spelled, and goes on when none does: link_refusal is a
# project of its own, because the refusal stops the whole configure.
function(ear_node_type_link_test name link)
	add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/link_refusal"
		-B "${CMAKE_CURRENT_BINARY_DIR}/${name}" "-DEAR_CMAKE_DIR=${PROJECT_SOURCE_DIR}/cmake" -DEAR_LINK=${link})
endfunction()
ear_node_type_link_test(ear_node_type_link_refused helper)
set_tests_properties(ear_node_type_link_refused PROPERTIES PASS_REGULAR_EXPRESSION "ear_node_a links ear_node_c")
foreach(link direct link-only alias generator-expression nested-generator-expression alias-in-generator-expression
		helper-in-generator-expression)
	ear_node_type_link_test(ear_node_type_link_refused_${link} ${link})
	set_tests_properties(ear_node_type_link_refused_${link} PROPERTIES
		PASS_REGULAR_EXPRESSION "ear_node_a links ear_node_c")
endforeach()
ear_node_type_link_test(ear_node_type_link_allowed none)

add_test(NAME ear_node_types_none_refused COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/no_types"
	-B "${CMAKE_CURRENT_BINARY_DIR}/ear_node_types_none_refused"
	"-DEAR_ALL_NODE_TYPES_DIR=${PROJECT_SOURCE_DIR}/src/all_node_types")
set_tests_properties(ear_node_types_none_refused PROPERTIES PASS_REGULAR_EXPRESSION "No node types found")

# No node type's own code writes standard output or error (docs/design/node-framework.md section 8.8): the guard over
# src/nodes/, and the guard itself refusing each spelling it knows (standard_output_guard/<spelling>.snippet) and
# letting through what only looks like one (allowed.snippet).
set(ear_guard "${CMAKE_CURRENT_SOURCE_DIR}/standard_output_guard.cmake")
add_test(NAME ear_nodes_never_write_standard_output
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${PROJECT_SOURCE_DIR}/src/nodes" -P "${ear_guard}")
foreach(spelling cout cerr clog stdout stderr stdout_fileno printf puts perror print println write1 write2 dprintf)
	add_test(NAME ear_standard_output_guard_refuses_${spelling}
		COMMAND "${CMAKE_COMMAND}" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}/standard_output_guard"
			"-DGLOB=${spelling}.snippet" -P "${ear_guard}")
	set_tests_properties(ear_standard_output_guard_refuses_${spelling} PROPERTIES
		PASS_REGULAR_EXPRESSION "${spelling}\\.snippet:1: writes standard output or error")
endforeach()
add_test(NAME ear_standard_output_guard_allows_lookalikes
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}/standard_output_guard"
		"-DGLOB=allowed.snippet" -P "${ear_guard}")
add_test(NAME ear_standard_output_guard_refuses_checking_nothing
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}/standard_output_guard"
		"-DGLOB=no-such-file.snippet" -P "${ear_guard}")
set_tests_properties(ear_standard_output_guard_refuses_checking_nothing PROPERTIES
	PASS_REGULAR_EXPRESSION "the guard would check nothing")
