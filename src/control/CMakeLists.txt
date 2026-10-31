# ear_control: the control surface, pure (docs/design/batch2.md section 1): the graph file
# (ear.json v2) and M1's status lines; the plain types of ear.conf's state on the wire (places and
# diagnostics); the one decoder and encoder of node and route JSON; the
# JSON-RPC protocol (envelope, commands, events, sessions, subscriptions); and MainLoopGateway,
# the one crossing between the main loop and the control thread. No sockets and no threads of
# its own: ear_server carries its messages.
add_library(ear_control STATIC
	commands.cpp
	events.cpp
	gateway.cpp
	graph_file.cpp
	graph_file_state.cpp
	graph_json.cpp
	rpc.cpp
	session.cpp
	status_log.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_control PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/control/commands.hpp
	include/ear/control/events.hpp
	include/ear/control/gateway.hpp
	include/ear/control/graph_file.hpp
	include/ear/control/graph_file_state.hpp
	include/ear/control/graph_json.hpp
	include/ear/control/rpc.hpp
	include/ear/control/session.hpp
	include/ear/control/status_log.hpp)
set_target_properties(ear_control PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_control PUBLIC ear_core)

if(BUILD_TESTS)
	include(Catch)
	add_executable(ear_control_tests
		tests/commands_test.cpp
		tests/events_test.cpp
		tests/gateway_test.cpp
		tests/graph_file_test.cpp
		tests/graph_file_state_test.cpp
		tests/graph_json_test.cpp
		tests/rpc_test.cpp
		tests/session_test.cpp)
	target_link_libraries(ear_control_tests PRIVATE ear_control ear_catch_main)
	# The wire format's fixtures, shared with the web UI's tests (docs/design/batch2.md section 6).
	target_compile_definitions(ear_control_tests PRIVATE
		EAR_PROTOCOL_DIR="${PROJECT_SOURCE_DIR}/test/protocol")
	add_dependencies(ear_control_tests ear_control_verify_interface_header_sets)
	catch_discover_tests(ear_control_tests)
endif()
