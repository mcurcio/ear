# Every C function ear gives PipeWire or alsa-lib goes through the engine's callback boundary
# (docs/architecture.md#the-callback-boundary): the guard over src/, and the guard itself refusing each way to hand one
# over that it knows (callback_guard/<spelling>.snippet), letting through what only looks like one (allowed.snippet),
# skipping exactly the exempt paths and tests (boundary/callback_boundary.snippet, tests/excluded.snippet) but not a
# file merely named like the boundary (elsewhere/callback_boundary.snippet), and refusing to check nothing.
set(ear_callback_guard "${CMAKE_CURRENT_SOURCE_DIR}/callback_guard.cmake")
set(ear_callback_snippets "${CMAKE_CURRENT_SOURCE_DIR}/callback_guard")
set(ear_callback_exempt "-DEXEMPT=boundary/callback_boundary.snippet")
add_test(NAME ear_callbacks_through_the_boundary
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${PROJECT_SOURCE_DIR}/src" -P "${ear_callback_guard}")
foreach(spelling pw_loop_add_event pw_loop_add_timer pw_loop_add_signal spa_loop_add_source spa_loop_utils_add
		pw_loop_invoke pw_data_loop_invoke spa_loop_invoke pw_loop_locked spa_loop_locked pw_work_queue_add
		filter_listener registry_listener object_listener spa_hook_list_append pw_stream_new_simple
		pw_filter_new_simple spa_interface_init spa_callbacks_init spa_callbacks log_methods pw_log_set
		snd_lib_error_set_handler split_listener)
	add_test(NAME ear_callback_guard_refuses_${spelling}
		COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_callback_snippets}" "-DGLOB=${spelling}.snippet"
			-P "${ear_callback_guard}")
	set_tests_properties(ear_callback_guard_refuses_${spelling} PROPERTIES
		PASS_REGULAR_EXPRESSION "${spelling}\\.snippet:1: registers a callback outside the boundary")
endforeach()
add_test(NAME ear_callback_guard_allows_lookalikes
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_callback_snippets}" "-DGLOB=allowed.snippet" -P "${ear_callback_guard}")
# The exempt path and a tests/ folder hold registrations, and are skipped: only allowed.snippet is checked.
add_test(NAME ear_callback_guard_skips_the_boundary_and_tests
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_callback_snippets}"
		"-DGLOB=allowed.snippet\;boundary/callback_boundary.snippet\;excluded.snippet" "${ear_callback_exempt}"
		-P "${ear_callback_guard}")
set_tests_properties(ear_callback_guard_skips_the_boundary_and_tests PROPERTIES
	PASS_REGULAR_EXPRESSION "callback_guard: 1 files under .*, none registers")
# Only the exact path is exempt: a file of the same name elsewhere is checked.
add_test(NAME ear_callback_guard_refuses_a_namesake_of_the_boundary
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_callback_snippets}" "-DGLOB=elsewhere/callback_boundary.snippet"
		"${ear_callback_exempt}" -P "${ear_callback_guard}")
set_tests_properties(ear_callback_guard_refuses_a_namesake_of_the_boundary PROPERTIES
	PASS_REGULAR_EXPRESSION "elsewhere/callback_boundary\\.snippet:2: registers a callback outside the boundary")
add_test(NAME ear_callback_guard_refuses_checking_nothing
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_callback_snippets}" "-DGLOB=no-such-file.snippet"
		-P "${ear_callback_guard}")
set_tests_properties(ear_callback_guard_refuses_checking_nothing PROPERTIES
	PASS_REGULAR_EXPRESSION "the guard would check nothing")
# Only skipped files: nothing is checked, which fails too.
add_test(NAME ear_callback_guard_refuses_checking_only_skipped_files
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_callback_snippets}"
		"-DGLOB=boundary/callback_boundary.snippet\;excluded.snippet" "${ear_callback_exempt}"
		-P "${ear_callback_guard}")
set_tests_properties(ear_callback_guard_refuses_checking_only_skipped_files PROPERTIES
	PASS_REGULAR_EXPRESSION "the guard would check nothing")
