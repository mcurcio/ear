# ear_system_tests: the owner's end-to-end tests. They spawn the real out/ear (the copy the
# ear target's post-build step makes, beside engine.conf) on a graph file, and read back the
# WAV file it wrote with libsndfile.
include(Catch)
add_executable(ear_system_tests white_to_file_test.cpp)
target_link_libraries(ear_system_tests PRIVATE ear_catch_main nlohmann_json::nlohmann_json
	PkgConfig::SNDFILE)
target_compile_definitions(ear_system_tests PRIVATE EAR_BINARY="${PROJECT_BINARY_DIR}/ear")
add_dependencies(ear_system_tests ear)
catch_discover_tests(ear_system_tests)
