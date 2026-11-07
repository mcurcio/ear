# ear_node_file: the "file" node type (docs/design/m1-white-to-file.md section 2), which writes WAV files with
# libsndfile beneath the recordings folder (ear_folder's confined folder). The only package that links libsndfile,
# privately.
ear_node_type(SOURCES file.cpp TESTS tests/file_test.cpp)
target_link_libraries(ear_node_file PRIVATE ear_folder PkgConfig::SNDFILE)

if(BUILD_TESTS)
	# libsndfile: the tests read back the WAV files the node wrote.
	target_link_libraries(ear_node_file_tests PRIVATE ear_folder PkgConfig::SNDFILE ear_temp_folder)
endif()
