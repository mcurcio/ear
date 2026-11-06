# ear_node_file: the "file" node type (docs/design/m1-white-to-file.md section 2), which writes WAV files with
# libsndfile beneath the recordings folder (ear_folder's confined folder). The only package that links libsndfile,
# privately.
add_library(ear_node_file STATIC file.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_node_file PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/ear/nodes/file.hpp)
set_target_properties(ear_node_file PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_node_file PUBLIC ear_core ear_folder PRIVATE PkgConfig::SNDFILE)

if(BUILD_TESTS)
	add_executable(ear_node_file_tests tests/file_test.cpp)
	# libsndfile: the tests read back the WAV files the node wrote.
	target_link_libraries(ear_node_file_tests PRIVATE ear_node_file ear_catch_main PkgConfig::SNDFILE)
	add_dependencies(ear_node_file_tests ear_node_file_verify_interface_header_sets)
	ear_catch_tests(ear_node_file_tests)
endif()
