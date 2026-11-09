# ear_pipewire: PipeWire's engine inside EAR, and the adapters that implement ear_core's
# interfaces with it: port blocks (with their levels), links, live ports, handoffs, sound cards,
# RTP receivers and the main-loop executor (docs/architecture.md#packages, docs/node-types.md#rtp).
# engine_names.hpp, link_proxy.hpp, engine_companions.hpp, latency_spa.hpp, engine_properties.hpp (every PipeWire
# property ear sets, docs/engine.md#properties-ear-sets), engine_property_set.hpp (the only writer of them) and
# callback_boundary.hpp (every C callback ear gives PipeWire, docs/architecture.md#the-callback-boundary) are private to
# this package: its sources include them by path.
add_library(ear_pipewire STATIC
	callback_boundary.cpp
	engine.cpp
	engine_companions.cpp
	engine_log.cpp
	engine_property_set.cpp
	handoffs.cpp
	links.cpp
	live_graph.cpp
	main_loop_executor.cpp
	port_blocks.cpp
	rtp_receivers.cpp
	sound_cards.cpp
	sources.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt. The headers
# declare PipeWire's types they name, so PipeWire's own headers stay private to this package.
target_sources(ear_pipewire PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/pipewire/data_loop_wedge.hpp
	include/ear/pipewire/engine.hpp
	include/ear/pipewire/engine_log.hpp
	include/ear/pipewire/faults.hpp
	include/ear/pipewire/handoffs.hpp
	include/ear/pipewire/links.hpp
	include/ear/pipewire/live_graph.hpp
	include/ear/pipewire/main_loop_executor.hpp
	include/ear/pipewire/port_blocks.hpp
	include/ear/pipewire/rtp_receivers.hpp
	include/ear/pipewire/sound_cards.hpp
	include/ear/pipewire/sources.hpp)
set_target_properties(ear_pipewire PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_pipewire PUBLIC ear_core PRIVATE PkgConfig::PIPEWIRE)

# The engine's configuration. The ear binary reads the copy beside out/ear (main passes
# <its directory>/engine.conf; Engine does no lookup of its own). configure_file copies it
# again whenever the source changes.
configure_file(engine.conf "${PROJECT_BINARY_DIR}/engine.conf" COPYONLY)

if(BUILD_TESTS)
	add_executable(ear_pipewire_tests
		tests/allocation_count.cpp
		tests/callback_boundary_test.cpp
		tests/engine_log_test.cpp
		tests/engine_properties_test.cpp
		tests/engine_test.cpp
		tests/handoff_test.cpp
		tests/latency_spa_test.cpp
		tests/main_loop_executor_test.cpp
		tests/port_block_test.cpp
		tests/rtp_receiver_test.cpp
		tests/sink_test.cpp
		tests/sound_card_test.cpp
		tests/source_test.cpp
		tests/spikes_test.cpp)
	# PipeWire directly: the tests drive the engine's loops and sync with its core themselves.
	# ear_rtp_tone: the RTP receiver tests' sender and tone fit; ear_temp_folder: their temporary folders;
	# ear_type_harness: ear/testing/drive_now.hpp, a block driven and waited for; ear_output: the main-loop watchdog
	# (StallWatch) over the real main loop, as main.cpp wires it; ear_inducers: planned data-thread stalls and the cycle
	# ledger (docs/testing.md#making-the-rare-event); ear_latency_table: the latency model's rows, run through a real
	# block.
	target_link_libraries(ear_pipewire_tests PRIVATE ear_pipewire ear_catch_main
		PkgConfig::PIPEWIRE ear_rtp_tone ear_temp_folder ear_type_harness ear_output ear_inducers ear_latency_table)
	# The tests run the engine on the source tree's engine.conf; the sound card tests point
	# ALSA_CONFIG_PATH into tests/alsa, a test ALSA configuration (slice E2 writes it). The property table's tests read
	# docs/engine.md, whose tables must list its rows.
	target_compile_definitions(ear_pipewire_tests PRIVATE
		EAR_ENGINE_CONF="${CMAKE_CURRENT_SOURCE_DIR}/engine.conf"
		EAR_ENGINE_DOC="${PROJECT_SOURCE_DIR}/docs/engine.md"
		EAR_ALSA_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/alsa")
	add_dependencies(ear_pipewire_tests ear_pipewire_verify_interface_header_sets)
	# S2 and S5 (spikes_test.cpp, docs/engine.md#the-one-data-loop-and-invokes, both tagged [s2]) keep the data thread
	# busy at a 64-frame block under an invoke storm (S2 about 25 s; S5 about 5 s, overrunning its own cycles beside a
	# busy thread per core); beside them, the engine, RTP and sendspin clock tests miss their deadlines.
	ear_catch_tests(ear_pipewire_tests SERIAL [s2])

	# What must not compile about the callback boundary (docs/architecture.md#the-callback-boundary):
	# tests/compile_fail/callback_boundary_bad.cpp says what each case is. The regexes are the image's GCC's wording.
	set(bad_boundary "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/callback_boundary_bad.cpp")
	set(boundary_includes "-I${CMAKE_CURRENT_SOURCE_DIR}" "-I${CMAKE_CURRENT_SOURCE_DIR}/include"
		"-I$<JOIN:$<TARGET_PROPERTY:PkgConfig::PIPEWIRE,INTERFACE_INCLUDE_DIRECTORIES>,$<SEMICOLON>-I>")
	ear_compile_check(ear_callback_boundary_compile_fail_control "${bad_boundary}" ${boundary_includes})
	# As ear_compile_fail (test/CMakeLists.txt), with PipeWire's headers and this package's private ones.
	function(ear_callback_boundary_compile_fail case regex)
		string(MAKE_C_IDENTIFIER "${case}" macro)
		set(name ear_callback_boundary_compile_fail_${case})
		ear_compile_check(${name} "${bad_boundary}" ${boundary_includes} -DEAR_CASE_${macro})
		set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
	endfunction()
	ear_callback_boundary_compile_fail(raw-event "no matching function for call to 'addEvent\\(pw_loop\\*&, void \\(\\*\\)")
	ear_callback_boundary_compile_fail(raw-invoke "no matching function for call to 'invoke\\(pw_loop\\*&, int \\(\\*\\)")
	ear_callback_boundary_compile_fail(raw-listener-entry "could not convert '[^']*rawState' from 'void \\(\\*\\)")
	ear_callback_boundary_compile_fail(guarded-process "could not convert '[^']*guarded<.* to '[^']*RealtimeSlot<")
	ear_callback_boundary_compile_fail(realtime-not-noexcept "the data loop never throws")
	ear_callback_boundary_compile_fail(other-class "cannot convert '[^']*Other\\*' to '[^']*::Class\\*'")
	ear_callback_boundary_compile_fail(wrong-signature "no matching function for call to 'addTimer")
	ear_callback_boundary_compile_fail(guarded-invoke-on-data-loop "no matching function for call to 'invoke\\(.*DataLoop")
	ear_callback_boundary_compile_fail(realtime-invoke-on-main-loop "no matching function for call to 'invoke\\(pw_loop\\*&, const ear::pipewire::Realtime<")
	ear_callback_boundary_compile_fail(realtime-timer "no matching function for call to 'addTimer\\(pw_loop\\*&, const ear::pipewire::Realtime<")
	ear_callback_boundary_compile_fail(realtime-in-guarded-entry "could not convert '[^']*realtime<.* to '[^']*GuardedSlot<")
	ear_callback_boundary_compile_fail(raw-log-method "could not convert '[^']*logNoexcept' from")
	ear_callback_boundary_compile_fail(log-method-not-noexcept "must be noexcept: the data loop never throws")
	# What must not compile about the property table (docs/engine.md#properties-ear-sets):
	# tests/compile_fail/engine_properties_bad.cpp says what each case is. property() refuses a key by calling a function
	# that is not constexpr, named for the reason, so each regex is that function's name, which the compiler's error
	# names.
	set(bad_properties "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/engine_properties_bad.cpp")
	ear_compile_check(ear_engine_properties_compile_fail_control "${bad_properties}" "-I${CMAKE_CURRENT_SOURCE_DIR}")
	function(ear_engine_properties_compile_fail case regex)
		string(MAKE_C_IDENTIFIER "${case}" macro)
		set(name ear_engine_properties_compile_fail_${case})
		ear_compile_check(${name} "${bad_properties}" "-I${CMAKE_CURRENT_SOURCE_DIR}" -DEAR_CASE_${macro})
		set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
	endfunction()
	ear_engine_properties_compile_fail(unlisted-key "notInThePropertyTable")
	ear_engine_properties_compile_fail(other-home "notInThePropertyTable")
	ear_engine_properties_compile_fail(forbidden-key "forbiddenOnThisHome")
	ear_engine_properties_compile_fail(string-key "could not convert '[^']*\"node.latency\"' from 'const char\\*' to '[^']*PropertyKey'")
	ear_engine_properties_compile_fail(pointer-key "PropertyKey\\(const char\\*\\)' is private")
	add_test(NAME ear_engine_conf_beside_ear COMMAND ${CMAKE_COMMAND} -E compare_files
		"${CMAKE_CURRENT_SOURCE_DIR}/engine.conf" "${PROJECT_BINARY_DIR}/engine.conf")
endif()
