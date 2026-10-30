# EAR's packages, one directory each with its own CMakeLists.txt (docs/design/batch2.md
# section 1): ear_core, ear_nodes, ear_pipewire, ear_control and ear_server, each a library the
# ear binary links (its composition root, main.cpp). Added in dependency order: ear_core first,
# the packages that use it after.
add_subdirectory(core)
add_subdirectory(nodes)
add_subdirectory(pipewire)
add_subdirectory(control)
add_subdirectory(server)

add_executable(ear main.cpp)
# The packages; program_options parses the command line.
target_link_libraries(ear PRIVATE ear_control ear_nodes ear_pipewire ear_server
	Boost::program_options)

# The binary is also copied to out/ear, the path the docs and CI run.
add_custom_command(TARGET ear POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ear> ${PROJECT_BINARY_DIR}/ear)

# The system tests spawn out/ear itself. Their directory is outside src/, so it is named with
# its build directory.
if(BUILD_TESTS)
	add_subdirectory("${PROJECT_SOURCE_DIR}/test/system" "${PROJECT_BINARY_DIR}/test/system")
endif()
