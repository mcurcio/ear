# ear_folder: the confined folder (docs/design/knobly-config.md), one directory EAR reads and writes
# beneath, held open for EAR's life: the recordings folder that "file" nodes write into, and the
# graph file folder that holds ear.conf. The kernel resolves every path inside it (openat2), and a
# file is replaced only if it still holds what the caller expected (renameat2). It depends on
# nothing of EAR's.
add_library(ear_folder STATIC
	confined_folder.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_folder PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/folder/confined_folder.hpp)
set_target_properties(ear_folder PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)

if(BUILD_TESTS)
	add_executable(ear_folder_tests
		tests/confined_folder_test.cpp)
	target_link_libraries(ear_folder_tests PRIVATE ear_folder ear_catch_main)
	add_dependencies(ear_folder_tests ear_folder_verify_interface_header_sets)
	ear_catch_tests(ear_folder_tests)
endif()
