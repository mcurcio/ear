# ear_runtime: the threads the node framework runs work on, behind ear_core's interfaces (docs/architecture.md#packages):
# the I/O thread (IoThread, core's IoExecutor), the one thread for blocking work, so a slow disk delays no node and never
# the main loop; and the nodes' loops (Loops, core's LoopAdapter): strands on one pool, a dedicated loop on a thread of
# its own; and the nodes' stores on disk (ConfinedStores, core's StoreAdapter, over ear_folder's graph file folder) with
# the boot sweep of every declared store; the nodes' recordings on disk (ConfinedRecordings, core's RecordingAdapter,
# over the recordings folder); and the kernel's word on a port a claim names (KernelPortProbe, core's
# PortProbe). It holds no policy (what a job means, when a store is trusted, is core's or
# its caller's) and no PipeWire. main.cpp makes it; no node type links it (configure refuses it, cmake/NodeType.cmake):
# types reach it only through core.
add_library(ear_runtime STATIC
	io_thread.cpp
	loops.cpp
	port_probe.cpp
	recordings.cpp
	stores.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_runtime PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/runtime/io_thread.hpp
	include/ear/runtime/loops.hpp
	include/ear/runtime/port_probe.hpp
	include/ear/runtime/recordings.hpp
	include/ear/runtime/stores.hpp)
set_target_properties(ear_runtime PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_runtime PUBLIC ear_core PRIVATE ear_folder)

if(BUILD_TESTS)
	add_executable(ear_runtime_tests
		tests/io_thread_test.cpp
		tests/loops_test.cpp
		tests/port_probe_test.cpp
		tests/recordings_test.cpp
		tests/stores_test.cpp)
	target_link_libraries(ear_runtime_tests PRIVATE ear_runtime ear_folder ear_temp_folder ear_catch_main)
	add_dependencies(ear_runtime_tests ear_runtime_verify_interface_header_sets)
	ear_catch_tests(ear_runtime_tests)
endif()
