# ear_nodes: the built-in node types, white, file, card and rtp (docs/design/batch2.md section 1,
# docs/design/rtp.md). They reach the engine only through ear_core's interfaces (a card or an
# RTP receiver is asked for through the node's grant); the file type writes WAV files with
# libsndfile, beneath the recordings folder (ear_folder's confined folder).
add_library(ear_nodes STATIC
	builtin.cpp
	card.cpp
	file.cpp
	rtp.cpp
	white.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_nodes PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/nodes/builtin.hpp)
set_target_properties(ear_nodes PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_nodes PUBLIC ear_core ear_folder PRIVATE PkgConfig::SNDFILE)

if(BUILD_TESTS)
	include(Catch)
	add_executable(ear_nodes_tests
		tests/card_test.cpp
		tests/file_test.cpp
		tests/rtp_test.cpp
		tests/white_test.cpp)
	# libsndfile: the file tests read back the WAV files the node wrote.
	target_link_libraries(ear_nodes_tests PRIVATE ear_nodes ear_catch_main PkgConfig::SNDFILE)
	add_dependencies(ear_nodes_tests ear_nodes_verify_interface_header_sets)
	catch_discover_tests(ear_nodes_tests)
endif()
