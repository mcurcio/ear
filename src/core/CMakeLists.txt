# ear_core: the desired graph and its edits, the node catalogue contract, the node framework, the
# reconciler, levels, the graph read and the executor contract (docs/design/batch2.md section 1).
# It never calls PipeWire, opens a socket, starts a thread or touches a file: nlohmann/json is its
# only dependency, and the engine and the control port reach it through the interfaces its
# headers declare.
add_library(ear_core STATIC
	command_error.cpp
	desired_graph.cpp
	graph_status.cpp
	levels.cpp
	node_catalogue.cpp
	node_host.cpp
	node_lifecycle.cpp
	ports.cpp
	reconciler.cpp
	rtp_receiver.cpp
	settings_table.cpp
	sound_card.cpp)
# The public headers as a header set: its base directory is the include path of ear_core and
# of everything that links it, and VERIFY_INTERFACE_HEADER_SETS adds a target that compiles
# each header on its own, so none depends on what an includer happened to include first. The
# test executable depends on that target, so "make test" builds it.
target_sources(ear_core PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/core/command_error.hpp
	include/ear/core/desired_graph.hpp
	include/ear/core/executor.hpp
	include/ear/core/frame_ring.hpp
	include/ear/core/graph_spec.hpp
	include/ear/core/graph_view.hpp
	include/ear/core/levels.hpp
	include/ear/core/node_catalogue.hpp
	include/ear/core/node_host.hpp
	include/ear/core/node_lifecycle.hpp
	include/ear/core/node_type.hpp
	include/ear/core/reconciler.hpp
	include/ear/core/rtp_receiver.hpp
	include/ear/core/settings_table.hpp
	include/ear/core/sound_card.hpp)
set_target_properties(ear_core PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_core PUBLIC nlohmann_json::nlohmann_json)

if(BUILD_TESTS)
	add_executable(ear_core_tests
		tests/desired_graph_test.cpp
		tests/frame_ring_test.cpp
		tests/graph_spec_test.cpp
		tests/graph_status_test.cpp
		tests/levels_test.cpp
		tests/node_host_test.cpp
		tests/node_lifecycle_test.cpp
		tests/ports_test.cpp
		tests/reconciler_test.cpp
		tests/rtp_receiver_test.cpp
		tests/settings_table_test.cpp
		tests/sound_card_test.cpp)
	target_link_libraries(ear_core_tests PRIVATE ear_core ear_catch_main)
	add_dependencies(ear_core_tests ear_core_verify_interface_header_sets)
	ear_catch_tests(ear_core_tests)

	# What must not compile, each with the reason it must not: tests/compile_fail/settings_table_bad.cpp says what each
	# case is. Most regexes are the wording of the image's GCC, so a compiler bump may need them edited; the control
	# guards the setup.
	set(bad_settings "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/settings_table_bad.cpp")
	ear_compile_check(ear_settings_table_compile_fail_control "${bad_settings}")
	function(ear_settings_table_compile_fail case regex)
		ear_compile_fail(ear_settings_table_compile_fail_${case} "${bad_settings}" ${case} "${regex}")
	endfunction()
	ear_settings_table_compile_fail(key-empty "a setting's key is empty")
	ear_settings_table_compile_fail(key-twice "a setting's key is given twice")
	ear_settings_table_compile_fail(name-empty "a table's name is empty")
	ear_settings_table_compile_fail(low-above-high "low is above its high")
	ear_settings_table_compile_fail(number-without-range "too few arguments")
	ear_settings_table_compile_fail(number-with-mustBe "too many arguments")
	ear_settings_table_compile_fail(default-outside-range "byDefault is outside its range")
	ear_settings_table_compile_fail(default-on-text "could not convert")
	ear_settings_table_compile_fail(default-on-required "no matching function for call to 'required")
	ear_settings_table_compile_fail(text-without-mustBe "needs what it must be")
	ear_settings_table_compile_fail(text-without-meaning "text<nullptr>")
	ear_settings_table_compile_fail(text-with-range "no matching function for call to 'text<")
	ear_settings_table_compile_fail(text-viewing-value "no matching function for call to 'text<")
	ear_settings_table_compile_fail(only-without-lower "must name a row above")
	ear_settings_table_compile_fail(raw-row "constraint failure")
	ear_settings_table_compile_fail(read-unlisted-row "no matching function for call to .*get<")
	ear_settings_table_compile_fail(read-number-as-text "conversion from")
	ear_settings_table_compile_fail(read-optional-unchecked "cannot convert")
	ear_settings_table_compile_fail(override-parse "parse.*marked 'override'")
	ear_settings_table_compile_fail(override-setting-kinds "settingKinds.*marked 'override'")
	ear_settings_table_compile_fail(override-name "name.*marked 'override'")
	ear_settings_table_compile_fail(make-takes-raw-settings "make.*marked 'override'")
	ear_settings_table_compile_fail(override-make-config "overriding final function")
	ear_settings_table_compile_fail(derive-without-table "private within this context")
	ear_settings_table_compile_fail(forged-checked-settings "no matching function for call to .*CheckedSettings")
	ear_settings_table_compile_fail(forged-row "private within this context")
endif()
