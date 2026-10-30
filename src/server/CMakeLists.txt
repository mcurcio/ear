# ear_server: the control thread and port 7777 (docs/design/batch2.md section 1): HTTP for the
# graph editor's files, WebSocket JSON-RPC at /rpc, the Host and Origin checks, per-client queues
# and the level bell. It holds no graph state and never calls PipeWire: the graph reaches it only
# through ear_control's GraphGateway. Boost.Beast and Asio (header-only) are PRIVATE: no Asio type
# is in a public header.
add_library(ear_server STATIC
	control_port.cpp
	control_thread.cpp
	level_bell.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_server PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/server/control_port.hpp
	include/ear/server/control_thread.hpp)
set_target_properties(ear_server PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_server PUBLIC ear_control PRIVATE Boost::beast)

if(BUILD_TESTS)
	include(Catch)
	add_executable(ear_server_tests
		tests/control_port_test.cpp)
	# ear_ws_client (test/CMakeLists.txt): the tests' WebSocket client, on Boost.Beast.
	target_link_libraries(ear_server_tests PRIVATE ear_server ear_catch_main ear_ws_client)
	add_dependencies(ear_server_tests ear_server_verify_interface_header_sets)
	catch_discover_tests(ear_server_tests)
endif()
