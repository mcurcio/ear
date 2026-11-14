# Configure stops at a target that sees PipeWire's headers without the callback poison forced in
# (docs/architecture.md#the-callback-boundary), however it sees them, and goes on for a poisoned target, a listed test
# program and a program linking a static library that links PipeWire privately: poison_refusal is a project of its own,
# because the refusal stops the whole configure.
function(ear_raw_pipewire_test name sees)
	add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/poison_refusal"
		-B "${CMAKE_CURRENT_BINARY_DIR}/${name}" "-DEAR_CMAKE_DIR=${PROJECT_SOURCE_DIR}/cmake" -DEAR_SEES=${sees}
		"-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endfunction()
foreach(sees direct helper alias generator-expression include-directories)
	ear_raw_pipewire_test(ear_raw_pipewire_refused_${sees} ${sees})
	set_tests_properties(ear_raw_pipewire_refused_${sees} PROPERTIES
		PASS_REGULAR_EXPRESSION "RawCallbacks\\.cmake:[0-9]+ \\(message\\):[^a-z]*ear_user sees PipeWire's headers \\(through [^)]*\\) without")
endforeach()
ear_raw_pipewire_test(ear_raw_pipewire_refused_executable executable)
set_tests_properties(ear_raw_pipewire_refused_executable PROPERTIES
	PASS_REGULAR_EXPRESSION "RawCallbacks\\.cmake:[0-9]+ \\(message\\):[^a-z]*ear_program sees PipeWire's headers \\(through [^)]*\\) without")
foreach(sees none poisoned listed private-link)
	ear_raw_pipewire_test(ear_raw_pipewire_allowed_${sees} ${sees})
endforeach()

# Only src/pipewire/engine_property_set.{hpp,cpp}, whose functions take keys only from the property table, writes
# PipeWire properties or makes an object from them (docs/engine.md#properties-ear-sets): the guard over src/, and the
# guard itself refusing each name it knows (property_guard/<spelling>.snippet, a "//" inside a string, a call split
# across lines, a function pointer and each way to build a spa_dict among them),
# letting through the table's functions, reads and look-alikes in strings and comments (allowed.snippet), skipping
# exactly the exempt path and tests but not a namesake elsewhere, and refusing to check nothing.
set(ear_property_guard "${CMAKE_CURRENT_SOURCE_DIR}/property_guard.cmake")
set(ear_property_snippets "${CMAKE_CURRENT_SOURCE_DIR}/property_guard")
set(ear_property_exempt "-DEXEMPT=exempt/engine_property_set.cpp")
add_test(NAME ear_properties_from_the_table
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${PROJECT_SOURCE_DIR}/src" -P "${ear_property_guard}")
foreach(spelling pw_properties_new pw_properties_new_string_checked pw_properties_new_dict pw_properties_set
		pw_properties_setf pw_properties_setva pw_properties_update pw_properties_update_string pw_properties_add
		pw_filter_update_properties pw_context_load_module pw_impl_module_load spa_dict_item_init spa_dict_item_macro
		spa_dict_init spa_dict_item comment_in_string call_split pw_core_create_object function_pointer spa_dict_variable
		spa_dict_temporary spa_dict_compound spa_dict_item_pointer)
	add_test(NAME ear_property_guard_refuses_${spelling}
		COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_property_snippets}" "-DGLOB=${spelling}.snippet"
			-P "${ear_property_guard}")
	set_tests_properties(ear_property_guard_refuses_${spelling} PROPERTIES
		PASS_REGULAR_EXPRESSION "${spelling}\\.snippet:1: writes a PipeWire property outside engine_property_set")
endforeach()
add_test(NAME ear_property_guard_allows_the_table_and_reads
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_property_snippets}" "-DGLOB=allowed.snippet" -P "${ear_property_guard}")
set_tests_properties(ear_property_guard_allows_the_table_and_reads PROPERTIES
	PASS_REGULAR_EXPRESSION "property_guard: 1 files under .*, none writes a property outside engine_property_set")
add_test(NAME ear_property_guard_skips_the_writer_and_tests
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_property_snippets}"
		"-DGLOB=allowed.snippet\;exempt/engine_property_set.cpp\;tests/excluded.snippet" "${ear_property_exempt}"
		-P "${ear_property_guard}")
set_tests_properties(ear_property_guard_skips_the_writer_and_tests PROPERTIES
	PASS_REGULAR_EXPRESSION "property_guard: 1 files under .*, none writes a property outside engine_property_set")
add_test(NAME ear_property_guard_refuses_a_namesake_of_the_writer
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_property_snippets}" "-DGLOB=elsewhere/engine_property_set.cpp"
		"${ear_property_exempt}" -P "${ear_property_guard}")
set_tests_properties(ear_property_guard_refuses_a_namesake_of_the_writer PROPERTIES
	PASS_REGULAR_EXPRESSION "elsewhere/engine_property_set\\.cpp:2: writes a PipeWire property outside")
add_test(NAME ear_property_guard_refuses_checking_nothing
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_property_snippets}" "-DGLOB=no-such-file.snippet"
		-P "${ear_property_guard}")
set_tests_properties(ear_property_guard_refuses_checking_nothing PROPERTIES
	PASS_REGULAR_EXPRESSION "the guard would check nothing")
