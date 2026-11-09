# ear_pipewire: PipeWire's engine inside EAR, and the adapters that implement ear_core's
# interfaces with it: port blocks (with their levels), links, live ports, handoffs, sound cards,
# RTP receivers and the main-loop executor (docs/design/batch2.md section 1, docs/design/rtp.md).
# engine_names.hpp, link_proxy.hpp and outside_nodes.hpp are private to this package: its
# sources include them by path.
add_library(ear_pipewire STATIC
	engine.cpp
	engine_log.cpp
	handoffs.cpp
	links.cpp
	live_graph.cpp
	main_loop_executor.cpp
	outside_nodes.cpp
	port_blocks.cpp
	rtp_receivers.cpp
	sound_cards.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt. The headers
# declare PipeWire's types they name, so PipeWire's own headers stay private to this package.
target_sources(ear_pipewire PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/pipewire/data_loop_wedge.hpp
	include/ear/pipewire/engine.hpp
	include/ear/pipewire/engine_log.hpp
	include/ear/pipewire/handoffs.hpp
	include/ear/pipewire/links.hpp
	include/ear/pipewire/live_graph.hpp
	include/ear/pipewire/main_loop_executor.hpp
	include/ear/pipewire/port_blocks.hpp
	include/ear/pipewire/rtp_receivers.hpp
	include/ear/pipewire/sound_cards.hpp)
set_target_properties(ear_pipewire PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_pipewire PUBLIC ear_core PRIVATE PkgConfig::PIPEWIRE)

# The engine's configuration. The ear binary reads the copy beside out/ear (main passes
# <its directory>/engine.conf; Engine does no lookup of its own). configure_file copies it
# again whenever the source changes.
configure_file(engine.conf "${PROJECT_BINARY_DIR}/engine.conf" COPYONLY)

if(BUILD_TESTS)
	add_executable(ear_pipewire_tests
		tests/allocation_count.cpp
		tests/engine_log_test.cpp
		tests/engine_test.cpp
		tests/handoff_test.cpp
		tests/main_loop_executor_test.cpp
		tests/port_block_test.cpp
		tests/rtp_receiver_test.cpp
		tests/sink_test.cpp
		tests/sound_card_test.cpp
		tests/spikes_test.cpp)
	# PipeWire directly: the tests drive the engine's loops and sync with its core themselves.
	# ear_rtp_tone: the RTP receiver tests' sender and tone fit; ear_temp_folder: their temporary folders;
	# ear_type_harness: ear/testing/drive_now.hpp, a block driven and waited for; ear_output: the main-loop watchdog
	# (StallWatch) over the real main loop, as main.cpp wires it.
	target_link_libraries(ear_pipewire_tests PRIVATE ear_pipewire ear_catch_main
		PkgConfig::PIPEWIRE ear_rtp_tone ear_temp_folder ear_type_harness ear_output)
	# The tests run the engine on the source tree's engine.conf; the sound card tests point
	# ALSA_CONFIG_PATH into tests/alsa, a test ALSA configuration (slice E2 writes it).
	target_compile_definitions(ear_pipewire_tests PRIVATE
		EAR_ENGINE_CONF="${CMAKE_CURRENT_SOURCE_DIR}/engine.conf"
		EAR_ALSA_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/alsa")
	add_dependencies(ear_pipewire_tests ear_pipewire_verify_interface_header_sets)
	# S2 (spikes_test.cpp, docs/design/node-lifecycle.md 10.2) keeps the data thread busy for about 25 s at a 64-frame
	# block under an invoke storm; beside it, the engine, RTP and sendspin clock tests miss their deadlines.
	ear_catch_tests(ear_pipewire_tests SERIAL [s2])

	add_test(NAME ear_engine_conf_beside_ear COMMAND ${CMAKE_COMMAND} -E compare_files
		"${CMAKE_CURRENT_SOURCE_DIR}/engine.conf" "${PROJECT_BINARY_DIR}/engine.conf")
endif()
