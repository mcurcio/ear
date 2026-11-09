# ear_output: where ear's standard output goes (docs/design/node-framework.md section 8.8): the output thread, the one
# writer of the real standard output, behind a bounded queue the main loop never blocks on; and the capture of
# descriptors 1 and 2, pipes read by a thread of its own, installed at boot before any other thread starts; and the
# main-loop watchdog (StallWatch, docs/design/node-lifecycle.md 5.12), told where the main loop is by main.cpp. Every
# record's text is ear_control's (status_log.cpp, the one encoder); main.cpp joins them. It depends on nothing of
# EAR's.
add_library(ear_output STATIC
	descriptor_capture.cpp
	output_writer.cpp
	stall_watch.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_output PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/output/descriptor_capture.hpp
	include/ear/output/output_writer.hpp
	include/ear/output/stall_watch.hpp)
set_target_properties(ear_output PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)

if(BUILD_TESTS)
	add_executable(ear_output_tests
		tests/descriptor_capture_test.cpp
		tests/output_writer_test.cpp
		tests/stall_watch_test.cpp)
	target_link_libraries(ear_output_tests PRIVATE ear_output ear_catch_main)
	add_dependencies(ear_output_tests ear_output_verify_interface_header_sets)
	ear_catch_tests(ear_output_tests)
endif()
