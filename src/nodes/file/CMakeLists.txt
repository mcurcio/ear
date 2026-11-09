# ear_node_file: the "file" node type (docs/node-types.md#file), which writes WAV files with libsndfile into a recording
# the framework opens beneath the recordings folder (docs/writing-a-node-type.md#configure-and-createof). The only
# package that links libsndfile, privately.
ear_node_type(SOURCES file.cpp TESTS tests/file_test.cpp)
target_link_libraries(ear_node_file PRIVATE PkgConfig::SNDFILE)

if(BUILD_TESTS)
	# A real recordings folder behind the harness's recordings, and libsndfile to read back the WAV files written.
	target_link_libraries(ear_node_file_tests PRIVATE ear_folder PkgConfig::SNDFILE ear_temp_folder)
endif()
