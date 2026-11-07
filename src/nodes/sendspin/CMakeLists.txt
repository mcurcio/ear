# ear_node_sendspin: the "sendspin" node type (docs/design/sendspin.md): Music Assistant's audio out of a port block's
# outputs, over the protocol library sendspin-cpp. The only package that sees that library, privately, as ear_config
# is knobly's. It reaches the engine only through ear_core's interfaces. The library never touches a realtime thread
# or PipeWire's main loop: its own worker and notifier threads call it, and the audio crosses through the feed.
ear_node_type(
	SOURCES
		advertiser.cpp
		connection_watch.cpp
		feed.cpp
		key_store.cpp
		note_gate.cpp
		port_claims.cpp
		sendspin_code.cpp
		sendspin_type.cpp
		settings.cpp
		to_server.cpp
		worker.cpp
	TESTS
		tests/advertiser_test.cpp
		tests/clock_test.cpp
		tests/code_test.cpp
		tests/feed_test.cpp
		tests/keys_test.cpp
		tests/notes_test.cpp
		tests/pcm_test.cpp
		tests/port_claims_test.cpp
		tests/settings_test.cpp
		tests/to_server_test.cpp
		tests/watch_test.cpp)
target_link_libraries(ear_node_sendspin PRIVATE ear_folder sendspin)
target_include_directories(ear_node_sendspin SYSTEM PRIVATE "${mdns_SOURCE_DIR}")   # one header, advertiser.cpp's alone

if(BUILD_TESTS)
	# Its private headers by path; the engine and the test card for the clock's tests, which share the engine tests'
	# harness; the real server through ear_sendspin_server; the library for the key store's provider base.
	target_include_directories(ear_node_sendspin_tests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src/pipewire/tests")
	target_link_libraries(ear_node_sendspin_tests PRIVATE ear_folder ear_pipewire PkgConfig::PIPEWIRE ear_sendspin_server
		ear_temp_folder sendspin)
	target_compile_definitions(ear_node_sendspin_tests PRIVATE
		EAR_ENGINE_CONF="${PROJECT_SOURCE_DIR}/src/pipewire/engine.conf"
		EAR_ALSA_TEST_DIR="${PROJECT_SOURCE_DIR}/src/pipewire/tests/alsa")
endif()
