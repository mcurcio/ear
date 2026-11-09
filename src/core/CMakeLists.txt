# ear_core: the desired graph and its edits, the node catalogue contract, the node framework, the
# reconciler, levels, the graph read and the executor contract (docs/architecture.md#packages).
# It never calls PipeWire, opens a socket, starts a thread or touches a file: nlohmann/json is its
# only dependency, and the engine and the control port reach it through the interfaces its
# headers declare.
add_library(ear_core STATIC
	command_error.cpp
	desired_graph.cpp
	graph_status.cpp
	legacy_instance.cpp
	levels.cpp
	log.cpp
	main_loop_activity.cpp
	node_catalogue.cpp
	node_host.cpp
	node_lifecycle.cpp
	pass_through.cpp
	ports.cpp
	reconciler.cpp
	rtp_receiver.cpp
	settings_table.cpp
	sound_card.cpp
	source_ring.cpp)
# The public headers as a header set: its base directory is the include path of ear_core and
# of everything that links it, and VERIFY_INTERFACE_HEADER_SETS adds a target that compiles
# each header on its own, so none depends on what an includer happened to include first. The
# test executable depends on that target, so "make test" builds it.
target_sources(ear_core PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/core/command_error.hpp
	include/ear/core/companions.hpp
	include/ear/core/desired_graph.hpp
	include/ear/core/executor.hpp
	include/ear/core/frame_ring.hpp
	include/ear/core/graph_spec.hpp
	include/ear/core/graph_view.hpp
	include/ear/core/levels.hpp
	include/ear/core/lifecycle.hpp
	include/ear/core/log.hpp
	include/ear/core/main_loop_activity.hpp
	include/ear/core/node_catalogue.hpp
	include/ear/core/node_folders.hpp
	include/ear/core/node_host.hpp
	include/ear/core/node_lifecycle.hpp
	include/ear/core/node_type.hpp
	include/ear/core/pass_through.hpp
	include/ear/core/reconciler.hpp
	include/ear/core/rtp_receiver.hpp
	include/ear/core/settings_table.hpp
	include/ear/core/sink.hpp
	include/ear/core/sound_card.hpp
	include/ear/core/source.hpp
	include/ear/core/source_ring.hpp
	include/ear/core/timers.hpp
	include/ear/core/type_spec.hpp)
set_target_properties(ear_core PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_core PUBLIC nlohmann_json::nlohmann_json)

if(BUILD_TESTS)
	# ear_type_harness: running a node type without PipeWire (ear/testing/, docs/writing-a-node-type.md#testing-a-type),
	# for each type's own tests (ear_node_type() links it) and the all-type tests. Test support, so built with the tests
	# only; like ear_core it starts no thread of its own and touches no file.
	add_library(ear_type_harness STATIC testing/node_harness.cpp)
	target_sources(ear_type_harness PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
		include/ear/testing/drive_now.hpp
		include/ear/testing/node_harness.hpp
		include/ear/testing/type_harness.hpp)
	set_target_properties(ear_type_harness PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
	target_link_libraries(ear_type_harness PUBLIC ear_core)

	add_executable(ear_core_tests
		tests/desired_graph_test.cpp
		tests/frame_ring_test.cpp
		tests/graph_spec_test.cpp
		tests/graph_status_test.cpp
		tests/levels_test.cpp
		tests/main_loop_activity_test.cpp
		tests/node_host_test.cpp
		tests/node_lifecycle_test.cpp
		tests/ports_test.cpp
		tests/reconciler_test.cpp
		tests/rtp_receiver_test.cpp
		tests/settings_table_test.cpp
		tests/sound_card_test.cpp
		tests/source_ring_test.cpp
		tests/type_spec_test.cpp)
	target_link_libraries(ear_core_tests PRIVATE ear_core ear_type_harness ear_catch_main)
	add_dependencies(ear_core_tests ear_core_verify_interface_header_sets ear_type_harness_verify_interface_header_sets)
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
	ear_settings_table_compile_fail(key-reserved-disabled "disabled is the framework's key")
	ear_settings_table_compile_fail(key-reserved-disabled-family "disabled is the framework's key")
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
	ear_settings_table_compile_fail(derive-by-copy "use of deleted function")
	ear_settings_table_compile_fail(forged-checked-settings "private within this context")
	ear_settings_table_compile_fail(forged-row "private within this context")
	ear_settings_table_compile_fail(forged-empty-rule "no matching function for call to .*Rule")
	ear_settings_table_compile_fail(forged-number-rule "private within this context")
	ear_settings_table_compile_fail(forged-text-rule "private within this context")
	ear_settings_table_compile_fail(default-inside-rule "too many initializers")
	ear_settings_table_compile_fail(row-field-changed "has no member named 'required'")
	ear_settings_table_compile_fail(row-read-only "discards qualifiers")
	ear_settings_table_compile_fail(only-without-on-required "a required row cannot be onlyWithout")
	ear_settings_table_compile_fail(only-without-a-required "cannot name a required row")
	ear_settings_table_compile_fail(table-not-named-for-folder "a type's table is named for its folder")

	# What must not compile about a type spec, its class, the contexts its configure is handed, Io and the harness
	# (docs/writing-a-node-type.md#the-class; docs/writing-a-node-type.md#rules-the-framework-enforces):
	# tests/compile_fail/type_spec_bad.cpp says what each case is.
	set(bad_spec "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/type_spec_bad.cpp")
	ear_compile_check(ear_type_spec_compile_fail_control "${bad_spec}")
	function(ear_type_spec_compile_fail case regex)
		ear_compile_fail(ear_type_spec_compile_fail_${case} "${bad_spec}" ${case} "${regex}")
	endfunction()
	ear_type_spec_compile_fail(io-bank-the-spec-lacks "Io takes only its spec's banks")
	ear_type_spec_compile_fail(io-write-into-input "assignment of read-only location")
	ear_type_spec_compile_fail(io-misspelt-bank "'outt' was not declared in this scope")
	ear_type_spec_compile_fail(io-not-a-bank "no match for 'operator\\[\\]'")
	ear_type_spec_compile_fail(ports-bank-the-spec-lacks "ports\\(\\) names only its spec's banks")
	ear_type_spec_compile_fail(ports-bank-left-out "ports\\(\\) gives every bank of its spec, each once")
	ear_type_spec_compile_fail(ports-bank-twice "ports\\(\\) gives every bank of its spec, each once")
	ear_type_spec_compile_fail(pass-through-backwards "a pass-through copies an input bank to an output bank")
	ear_type_spec_compile_fail(bank-name-invalid "a bank's name is 1 or more of")
	ear_type_spec_compile_fail(banks-named-twice "a type's banks have unique names")
	ear_type_spec_compile_fail(process-may-throw "a code's process is void process")
	ear_type_spec_compile_fail(check-wrong-result "a type's check is static std::expected")
	ear_type_spec_compile_fail(check-takes-mutable-settings "a type's check is static std::expected")
	ear_type_spec_compile_fail(prepare-wrong-signature "a code's prepare is void prepare")
	ear_type_spec_compile_fail(release-may-throw "a code's release is void release\\(\\) noexcept")
	ear_type_spec_compile_fail(spec-in-unnamed-namespace "a spec is declared in a named namespace")
	ear_type_spec_compile_fail(row-in-unnamed-namespace "a table's rows are declared in a named namespace")
	ear_type_spec_compile_fail(spec-not-named-for-folder "a type's table is named for its folder")
	ear_type_spec_compile_fail(harness-bank-the-spec-lacks "the harness takes only its spec's banks")
	ear_type_spec_compile_fail(harness-output-as-input "input\\(\\) takes an input bank")
	# The class (docs/writing-a-node-type.md#rules-the-framework-enforces): its members' shapes, then what its
	# configure's context offers.
	ear_type_spec_compile_fail(class-without-ports "a type's class has static Members<spec> ports")
	ear_type_spec_compile_fail(configure-wrong-signature "a type's class has Configured configure")
	ear_type_spec_compile_fail(configure-not-a-member "a type's class has Configured configure")
	ear_type_spec_compile_fail(start-wrong-signature "a type's start is void start\\(\\)")
	ear_type_spec_compile_fail(stop-wrong-signature "a type's stop is void stop\\(\\)")
	ear_type_spec_compile_fail(constructor-neither "a type's class is constructed from NodeOf<spec>& or from nothing")
	ear_type_spec_compile_fail(legacy-start-on-spec-type "writes configure, not a static start")
	# The change version compiles in 3a (the owner's answer to Q1: the control writes one); of the wrong shape, not.
	ear_type_spec_compile_fail(change-version-wrong-signature "a type's change version is Changed configure")
	ear_type_spec_compile_fail(change-version-wrong-context "a type's change version is Changed configure")
	# A member found by name whatever its access or overloads: refused, never silently not called.
	ear_type_spec_compile_fail(private-start "a type's start is public and not overloaded")
	ear_type_spec_compile_fail(private-stop "a type's stop is public and not overloaded")
	ear_type_spec_compile_fail(private-check "a type's check is public and not overloaded")
	ear_type_spec_compile_fail(overloaded-start "a type's start is public and not overloaded")
	ear_type_spec_compile_fail(class-final "a type's class is a class, not final")
	ear_type_spec_compile_fail(create-has-no-hold "has no member named 'hold'")
	ear_type_spec_compile_fail(create-has-no-when-ready "has no member named 'whenReady'")
	ear_type_spec_compile_fail(create-has-no-unclaim "has no member named 'unclaim'")
	ear_type_spec_compile_fail(configured-has-no-restart "'restart' is not a member of 'ear::Configured'")
	ear_type_spec_compile_fail(sound-card-on-public-bank "a sound card joins an inside bank of the spec")
	ear_type_spec_compile_fail(rtp-receiver-on-output-bank "an RTP receiver joins an inside input bank of the spec")
	ear_type_spec_compile_fail(sink-on-input-bank "a sink reads an inside output bank of the spec")
	ear_type_spec_compile_fail(source-on-output-bank "a source plays into an inside input bank of the spec")
	ear_type_spec_compile_fail(source-on-routable-bank "a source plays into an inside input bank of the spec")
	ear_type_spec_compile_fail(declare-latency-bank-the-spec-lacks "declareLatency takes its spec's banks")
	ear_type_spec_compile_fail(declare-latency-backwards "declareLatency names a pair: an input bank, then an output bank")
	ear_type_spec_compile_fail(context-bank-the-spec-lacks "a context takes only its spec's banks")
endif()
