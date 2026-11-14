# ear_runtime: the threads the node framework runs work on, behind ear_core's interfaces (docs/architecture.md#packages):
# the I/O thread (IoThread, core's IoExecutor), the one thread for blocking work, so a slow disk delays no node and never
# the main loop; and the nodes' loops (Loops, core's LoopAdapter): strands on one pool, a dedicated loop on a thread of
# its own. It holds no policy (what a job means is core's or its caller's) and no PipeWire. main.cpp
# makes it; no node type links it (configure refuses it, cmake/NodeType.cmake): types reach it only through core.
add_library(ear_runtime STATIC
	io_thread.cpp
	loops.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_runtime PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/runtime/io_thread.hpp
	include/ear/runtime/loops.hpp)
set_target_properties(ear_runtime PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_runtime PUBLIC ear_core)

if(BUILD_TESTS)
	add_executable(ear_runtime_tests
		tests/io_thread_test.cpp
		tests/loops_test.cpp)
	target_link_libraries(ear_runtime_tests PRIVATE ear_runtime ear_catch_main)
	add_dependencies(ear_runtime_tests ear_runtime_verify_interface_header_sets)
	ear_catch_tests(ear_runtime_tests)
endif()
