# ear_system_tests: the owner's end-to-end tests. They spawn the real out/ear (the copy the
# ear target's post-build step makes, beside engine.conf) on a graph file folder (ear.conf), read back the WAV
# file it wrote with libsndfile, talk to its control port with ear_ws_client, send it RTP with
# ear_rtp_tone and play to its sendspin node from the real server with ear_sendspin_server.
add_executable(ear_system_tests control_port_test.cpp counters_test.cpp graph_file_test.cpp lifecycle_test.cpp rtp_test.cpp
	sendspin_connections_test.cpp sendspin_floods_test.cpp sendspin_limits_test.cpp sendspin_mdns_test.cpp
	sendspin_pairing_test.cpp sendspin_ramp_test.cpp sendspin_streams_test.cpp white_to_file_test.cpp)
target_link_libraries(ear_system_tests PRIVATE ear_catch_main nlohmann_json::nlohmann_json
	PkgConfig::SNDFILE ear_ws_client ear_rtp_tone ear_sendspin_server ear_temp_folder ear_statistics)
target_compile_definitions(ear_system_tests PRIVATE EAR_BINARY="${PROJECT_BINARY_DIR}/ear")
add_dependencies(ear_system_tests ear)
ear_catch_tests(ear_system_tests)
