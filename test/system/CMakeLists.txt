# ear_system_tests: the owner's end-to-end tests. They spawn the real out/ear (the copy the
# ear target's post-build step makes, beside engine.conf) on a graph file folder (ear.conf), read back the WAV
# file it wrote with libsndfile, talk to its control port with ear_ws_client, send it RTP with
# ear_rtp_tone, play to its sendspin node from the real server with ear_sendspin_server, and play to its airplay node
# from the scripted AirPlay 1 sender (test/support/airplay_sender.py, in the venv CMakeLists.txt checks for).
add_executable(ear_system_tests airplay_test.cpp control_port_test.cpp counters_test.cpp gain_test.cpp graph_file_test.cpp lifecycle_test.cpp rtp_test.cpp
	sendspin_changes_test.cpp sendspin_connections_test.cpp sendspin_floods_test.cpp sendspin_limits_test.cpp sendspin_mdns_test.cpp
	sendspin_pairing_test.cpp sendspin_ramp_test.cpp sendspin_status_test.cpp sendspin_streams_test.cpp
	white_to_file_test.cpp)
target_link_libraries(ear_system_tests PRIVATE ear_catch_main nlohmann_json::nlohmann_json
	PkgConfig::SNDFILE ear_ws_client ear_rtp_tone ear_sendspin_server ear_temp_folder ear_statistics)
target_compile_definitions(ear_system_tests PRIVATE EAR_BINARY="${PROJECT_BINARY_DIR}/ear"
	EAR_PROTOCOL_DIR="${PROJECT_SOURCE_DIR}/test/protocol"
	EAR_AIRPLAY_SENDER_PYTHON="${EAR_AIRPLAY_SENDER_PYTHON}"
	EAR_AIRPLAY_SENDER_SCRIPT="${PROJECT_SOURCE_DIR}/test/support/airplay_sender.py" EAR_NQPTP="${EAR_NQPTP}")
add_dependencies(ear_system_tests ear)
# The airplay tests run with nothing beside them: each plays a tone through shairport-sync, the pipe module's resampler
# and a file node, whose level a loaded host's stall dips below the floor they judge.
ear_catch_tests(ear_system_tests SERIAL "[airplay]")
