# EAR's packages, one directory each with its own CMakeLists.txt (docs/design/batch2.md
# section 1): ear_core, ear_pipewire, ear_control, ear_config (the knobly graph file and its keeper,
# docs/design/knobly-config.md) and ear_server, each a library the ear binary links (its composition root,
# main.cpp); nodes/ holds the node types, each its own library, ear_node_<type>, and all_node_types/ is the
# list of them configure generates, the one thing of theirs the binary links; ear_folder, the confined folder the
# recordings and the graph file live beneath, depends on none of them. Added in dependency order: ear_core and
# ear_folder first, the packages that use them after.
add_subdirectory(core)
add_subdirectory(folder)
add_subdirectory(nodes)
add_subdirectory(all_node_types)
add_subdirectory(pipewire)
add_subdirectory(control)
add_subdirectory(config)
add_subdirectory(server)

add_executable(ear main.cpp)
# The packages and the generated list of node types; ear_folder because main.cpp opens the folders; program_options
# parses the command line.
target_link_libraries(ear PRIVATE ear_config ear_control ear_folder ear_all_node_types ear_pipewire ear_server
	Boost::program_options)

# The binary is also copied to out/ear, the path the docs and CI run.
add_custom_command(TARGET ear POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ear> ${PROJECT_BINARY_DIR}/ear)

# The system tests spawn out/ear itself. Their directory is outside src/, so it is named with
# its build directory.
if(BUILD_TESTS)
	add_subdirectory("${PROJECT_SOURCE_DIR}/test/system" "${PROJECT_BINARY_DIR}/test/system")
endif()
