# ear_pipewire: PipeWire's engine inside EAR, and the adapters that implement ear_core's
# interfaces with it: port blocks (with their levels), links, live ports, handoffs, sound cards,
# module-run converters (RTP receivers) and the main-loop executor (docs/architecture.md#packages,
# docs/node-types.md#rtp).
# engine_names.hpp, link_proxy.hpp, engine_companions.hpp, converter_modules.hpp, module_converter_body.hpp,
# latency_spa.hpp, engine_properties.hpp (every PipeWire
# property ear sets, docs/engine.md#properties-ear-sets), engine_property_set.hpp (the only writer of them) and
# callback_boundary.hpp (every C callback ear gives PipeWire, docs/architecture.md#the-callback-boundary), with
# no_raw_callbacks.hpp (forced into every source here, below), and
# driver_cuts.hpp (the internal clock's cuts, docs/engine.md#graphcycle) are private to this package: its sources
# include them by path.
add_library(ear_pipewire STATIC
	callback_boundary.cpp
	driver_cuts.cpp
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
	include/ear/pipewire/module_converters.hpp
	include/ear/pipewire/port_blocks.hpp
	include/ear/pipewire/rtp_receivers.hpp
	include/ear/pipewire/sound_cards.hpp
	include/ear/pipewire/sources.hpp)
set_target_properties(ear_pipewire PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_pipewire PUBLIC ear_core PRIVATE PkgConfig::PIPEWIRE)

# Every C function ear gives PipeWire or alsa-lib goes through the callback boundary
# (docs/architecture.md#the-callback-boundary): no_raw_callbacks.hpp, forced into every source here, poisons each raw
# way to hand one over after the boundary's header, so it does not compile. Configure refuses any other target that sees
# PipeWire's headers without it, but the two test programs that drive PipeWire themselves.
include("${PROJECT_SOURCE_DIR}/cmake/RawCallbacks.cmake")
set(ear_no_raw_callbacks "${CMAKE_CURRENT_SOURCE_DIR}/no_raw_callbacks.hpp")
ear_poison_raw_callbacks(ear_pipewire "${ear_no_raw_callbacks}")
ear_refuse_raw_pipewire(PkgConfig::PIPEWIRE "${ear_no_raw_callbacks}" EXEMPT ear_pipewire_tests ear_node_sendspin_tests)

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
		tests/module_converter_test.cpp
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
	# ledger (docs/testing.md#making-the-rare-event); ear_cycle_judge: the one judge of lost cycles
	# (docs/testing.md#the-cycle-judge); ear_latency_table and ear_cycle_table: the latency model's rows and the cycle
	# facts' rows, run through a real block.
	target_link_libraries(ear_pipewire_tests PRIVATE ear_pipewire ear_catch_main PkgConfig::PIPEWIRE ear_rtp_tone
		ear_temp_folder ear_type_harness ear_output ear_inducers ear_cycle_judge ear_latency_table ear_cycle_table)
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
	ear_callback_boundary_compile_fail(raw-event "no matching function for call to 'addEvent\\([^']*LoopSource&, pw_loop\\*&, void \\(\\*\\)")
	ear_callback_boundary_compile_fail(raw-invoke "no matching function for call to 'invoke\\(pw_loop\\*&, int \\(\\*\\)")
	ear_callback_boundary_compile_fail(raw-listener-entry "could not convert '[^']*rawState' from 'void \\(\\*\\)")
	ear_callback_boundary_compile_fail(guarded-process "could not convert '[^']*guarded<.* to '[^']*RealtimeSlot<")
	ear_callback_boundary_compile_fail(realtime-not-noexcept "the data loop never throws")
	ear_callback_boundary_compile_fail(other-class "cannot convert '[^']*Other\\*' to '[^']*::Class\\*'")
	ear_callback_boundary_compile_fail(wrong-signature "no matching function for call to 'addTimer")
	ear_callback_boundary_compile_fail(guarded-invoke-on-data-loop "no matching function for call to 'invoke\\(.*DataLoop")
	ear_callback_boundary_compile_fail(realtime-invoke-on-main-loop "no matching function for call to 'invoke\\(pw_loop\\*&, const ear::pipewire::Realtime<")
	ear_callback_boundary_compile_fail(realtime-timer "no matching function for call to 'addTimer\\([^']*LoopSource&, pw_loop\\*&, const ear::pipewire::Realtime<")
	ear_callback_boundary_compile_fail(realtime-in-guarded-entry "could not convert '[^']*realtime<.* to '[^']*GuardedSlot<")
	ear_callback_boundary_compile_fail(raw-log-method "could not convert '[^']*logNoexcept' from")
	ear_callback_boundary_compile_fail(log-method-not-noexcept "must be noexcept: the data loop never throws")
	ear_callback_boundary_compile_fail(guarded-node-event "could not convert '[^']*guarded<.* to '[^']*RealtimeSlot<")
	ear_callback_boundary_compile_fail(raw-node-event "could not convert '[^']*rawStart' from 'void \\(\\*\\)")
	# What must not compile outside the boundary (docs/architecture.md#the-callback-boundary): each raw way to hand
	# PipeWire a callback, refused by the poison no_raw_callbacks.hpp forces in after the boundary, as ear_pipewire's
	# sources are compiled; tests/compile_fail/no_raw_callbacks_bad.cpp says what each case is. The regexes are GCC's.
	set(bad_raw "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/no_raw_callbacks_bad.cpp")
	set(raw_flags ${boundary_includes} -include "${ear_no_raw_callbacks}")
	ear_compile_check(ear_no_raw_callbacks_compile_fail_control "${bad_raw}" ${raw_flags})
	function(ear_no_raw_callbacks_compile_fail case name)
		string(MAKE_C_IDENTIFIER "${case}" macro)
		set(test ear_no_raw_callbacks_compile_fail_${case})
		ear_compile_check(${test} "${bad_raw}" ${raw_flags} -DEAR_CASE_${macro})
		set_tests_properties(${test} PROPERTIES PASS_REGULAR_EXPRESSION "attempt to use poisoned \"${name}\"")
	endfunction()
	ear_no_raw_callbacks_compile_fail(raw-call pw_loop_add_event)
	ear_no_raw_callbacks_compile_fail(split-call pw_filter_add_listener)
	ear_no_raw_callbacks_compile_fail(pasted pw_loop_invoke)
	ear_no_raw_callbacks_compile_fail(spliced pw_core_add_listener)
	ear_no_raw_callbacks_compile_fail(macro-after pw_impl_node_add_rt_listener)
	ear_no_raw_callbacks_compile_fail(hook-by-hand spa_hook)
	ear_no_raw_callbacks_compile_fail(interface-by-hand SPA_INTERFACE_INIT)
	ear_no_raw_callbacks_compile_fail(walk pw_context_for_each_global)
	ear_no_raw_callbacks_compile_fail(by-name dlsym)
	ear_no_raw_callbacks_compile_fail(later-header pw_profiler_methods)
	# Every poisoned name in one compile, each reported on its own (tests/compile_fail/every_name.cmake).
	add_test(NAME ear_no_raw_callbacks_compile_fail_every_name
		COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${bad_raw}" -DCASE=EAR_CASE_every_name
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/every_name.cmake" --
			"${CMAKE_CXX_COMPILER}" ${CMAKE_CXX23_STANDARD_COMPILE_OPTION} -w -fsyntax-only
			"-I$<JOIN:$<TARGET_PROPERTY:ear_core,INTERFACE_INCLUDE_DIRECTORIES>,;-I>" ${raw_flags}
		COMMAND_EXPAND_LISTS)
	# What must not compile about the property table (docs/engine.md#properties-ear-sets):
	# tests/compile_fail/engine_properties_bad.cpp says what each case is. property() refuses a key by calling a function
	# that is not constexpr, named for the reason, so each regex is that function's name, which the compiler's error
	# names.
	set(bad_properties "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/engine_properties_bad.cpp")
	ear_compile_check(ear_engine_properties_compile_fail_control "${bad_properties}" ${boundary_includes})
	function(ear_engine_properties_compile_fail case regex)
		string(MAKE_C_IDENTIFIER "${case}" macro)
		set(name ear_engine_properties_compile_fail_${case})
		ear_compile_check(${name} "${bad_properties}" ${boundary_includes} -DEAR_CASE_${macro})
		set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
	endfunction()
	ear_engine_properties_compile_fail(unlisted-key "notInThePropertyTable")
	ear_engine_properties_compile_fail(other-home "notInThePropertyTable")
	ear_engine_properties_compile_fail(forbidden-key "forbiddenOnThisHome")
	ear_engine_properties_compile_fail(string-key "no matching function for call to '[^']*PropertySet<[^']*>::set\\(const char \\[")
	ear_engine_properties_compile_fail(pointer-key "PropertyKey<H>::PropertyKey\\(const char\\*\\) .* is private")
	ear_engine_properties_compile_fail(key-of-another-home
		"no matching function for call to '[^']*PropertySet<[^']*Home::block>::set\\([^']*PropertyKey<[^']*Home::card>")
	ear_engine_properties_compile_fail(seam-key-on-a-block
		"no matching function for call to '[^']*PropertySet<[^']*Home::block>::set\\([^']*PropertyKey<[^']*Home::cardSeam>")
	ear_engine_properties_compile_fail(seam-key-from-a-temporary "use of deleted function '[^']*seamChecked\\(std::string&&\\)'")
	ear_engine_properties_compile_fail(stream-arguments-elsewhere
		"no matching function for call to '[^']*ModuleArguments<[^']*Home::rtpStream>::add")
	add_test(NAME ear_engine_conf_beside_ear COMMAND ${CMAKE_COMMAND} -E compare_files
		"${CMAKE_CURRENT_SOURCE_DIR}/engine.conf" "${PROJECT_BINARY_DIR}/engine.conf")
endif()
