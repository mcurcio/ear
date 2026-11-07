# ear_node_file: the "file" node type (docs/design/m1-white-to-file.md section 2), which writes WAV files with
# libsndfile into a recording the framework opens beneath the recordings folder (docs/design/node-framework.md section
# 7.3). The only package that links libsndfile, privately.
ear_node_type(SOURCES file.cpp TESTS tests/file_test.cpp)
target_link_libraries(ear_node_file PRIVATE PkgConfig::SNDFILE)

if(BUILD_TESTS)
	# A real recordings folder behind the harness's recordings, and libsndfile to read back the WAV files written.
	target_link_libraries(ear_node_file_tests PRIVATE ear_folder PkgConfig::SNDFILE ear_temp_folder)
endif()
