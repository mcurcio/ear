# ear_node_file: the "file" node type (docs/node-types.md#file), which writes WAV files with libsndfile into a recording
# the framework claims and opens beneath the recordings folder (docs/writing-a-node-type.md#recordings). The only
# package that links libsndfile, privately.
ear_node_type(SOURCES file.cpp TESTS tests/file_test.cpp)
target_link_libraries(ear_node_file PRIVATE PkgConfig::SNDFILE)

if(BUILD_TESTS)
	# libsndfile to read back the WAV files the harness's in-memory recordings hold, through a temporary folder.
	target_link_libraries(ear_node_file_tests PRIVATE PkgConfig::SNDFILE ear_temp_folder)
endif()
