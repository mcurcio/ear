# ear_node_sendspin: the "sendspin" node type (docs/node-types.md#sendspin): Music Assistant's audio in and out over the
# protocol library sendspin-cpp. The only package that sees that library, privately, as ear_config is knobly's. It
# reaches the engine only through ear_core's interfaces. The library never touches a realtime thread or PipeWire's main
# loop: the node's two loops (the worker and the notifier) call it, and the audio crosses through the source, its clock and the sink.
ear_node_type(
	SOURCES
		connection_watch.cpp
		key_store.cpp
		sendspin.cpp
		sendspin_clock.cpp
		settings.cpp
		to_server.cpp
		worker.cpp
	TESTS
		tests/advertiser_test.cpp
		tests/clock_test.cpp
		tests/code_test.cpp
		tests/keys_test.cpp
		tests/notes_test.cpp
		tests/pcm_test.cpp
		tests/settings_test.cpp
		tests/to_server_test.cpp
		tests/transport_test.cpp
		tests/type_test.cpp
		tests/watch_test.cpp)
target_link_libraries(ear_node_sendspin PRIVATE sendspin ear_mdns) # ear_mdns: its listing (docs/node-types.md#mdns)

if(BUILD_TESTS)
	# Its private headers by path; the engine and the test card for the clock's tests, which share the engine tests'
	# harness, and ear_runtime's loops, on which they run the worker's and notifier's states on real threads; the real
	# server through ear_sendspin_server; the library for the key store's provider base; IXWebSocket for the transport's
	# share per address (ixwebsocket-limits.patch); ear_mdns for the worker's header; ear_inducers for the clock tests'
	# cycle ledger (docs/testing.md#making-the-rare-event), ear_cycle_judge for the one judge of their cycles
	# (docs/testing.md#the-cycle-judge) and ear_standard_error_lines for the library's own word
	# (docs/testing.md#the-librarys-own-word).
	target_include_directories(ear_node_sendspin_tests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src/pipewire/tests")
	target_link_libraries(ear_node_sendspin_tests PRIVATE ear_folder ear_pipewire ear_runtime PkgConfig::PIPEWIRE
		ear_sendspin_server ear_temp_folder ear_inducers ear_cycle_judge ear_standard_error_lines sendspin ixwebsocket
		ear_mdns)
	target_compile_definitions(ear_node_sendspin_tests PRIVATE
		EAR_ENGINE_CONF="${PROJECT_SOURCE_DIR}/src/pipewire/engine.conf"
		EAR_ALSA_TEST_DIR="${PROJECT_SOURCE_DIR}/src/pipewire/tests/alsa"
		# Where the stall tests append the burst shape each run judged (docs/testing.md#a-writers-own-event): beside
		# ctest's own records, which CI prints from and uploads.
		EAR_STALL_SHAPES_FILE="${PROJECT_BINARY_DIR}/Testing/sendspin-stall-shapes.txt")
endif()
