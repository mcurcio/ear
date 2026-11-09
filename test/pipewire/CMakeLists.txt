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

# Every PipeWire property ear sets takes its key from the property table (docs/engine.md#properties-ear-sets): the
# guard over src/, and the guard itself refusing each way to set one around the table that it knows
# (property_guard/<spelling>.snippet), letting through property(), bound names, reads and look-alikes (allowed.snippet),
# skipping tests (tests/excluded.snippet), and refusing to check nothing.
set(ear_property_guard "${CMAKE_CURRENT_SOURCE_DIR}/property_guard.cmake")
set(ear_property_snippets "${CMAKE_CURRENT_SOURCE_DIR}/property_guard")
add_test(NAME ear_properties_from_the_table
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${PROJECT_SOURCE_DIR}/src" -P "${ear_property_guard}")
foreach(spelling pw_properties_new pw_properties_new_later pw_properties_set pw_properties_setf spa_dict_item_init
		spa_dict_item pw_properties_new_string pw_properties_update_string pw_context_load_module)
	add_test(NAME ear_property_guard_refuses_${spelling}
		COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_property_snippets}" "-DGLOB=${spelling}.snippet"
			-P "${ear_property_guard}")
	set_tests_properties(ear_property_guard_refuses_${spelling} PROPERTIES
		PASS_REGULAR_EXPRESSION "${spelling}\\.snippet:1: sets a property outside the table")
endforeach()
add_test(NAME ear_property_guard_allows_the_table_and_reads
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_property_snippets}" "-DGLOB=allowed.snippet" -P "${ear_property_guard}")
set_tests_properties(ear_property_guard_allows_the_table_and_reads PROPERTIES
	PASS_REGULAR_EXPRESSION "property_guard: 1 files under .*, every property set from the table")
add_test(NAME ear_property_guard_skips_tests
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_property_snippets}" "-DGLOB=allowed.snippet\;tests/excluded.snippet"
		-P "${ear_property_guard}")
set_tests_properties(ear_property_guard_skips_tests PROPERTIES
	PASS_REGULAR_EXPRESSION "property_guard: 1 files under .*, every property set from the table")
add_test(NAME ear_property_guard_refuses_checking_nothing
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_property_snippets}" "-DGLOB=no-such-file.snippet"
		-P "${ear_property_guard}")
set_tests_properties(ear_property_guard_refuses_checking_nothing PROPERTIES
	PASS_REGULAR_EXPRESSION "the guard would check nothing")
