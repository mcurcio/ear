# EAR's packages, one directory each with its own CMakeLists.txt (docs/design/m1-white-to-file.md
# section 1): ear_core, ear_nodes, ear_pipewire and ear_control, each a library the ear binary
# links (its composition root, main.cpp).
#
# ---- Packages: slice 0 adds them here, in dependency order ----
# add_subdirectory(core)
# add_subdirectory(nodes)
# add_subdirectory(pipewire)
# add_subdirectory(control)
# ---- end of packages ----

add_executable(ear main.cpp)
# program_options parses the command line; the packages above join this list as they arrive.
target_link_libraries(ear PRIVATE Boost::program_options)

# The binary is also copied to out/ear, the path the docs and CI run.
add_custom_command(TARGET ear POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ear> ${PROJECT_BINARY_DIR}/ear)
