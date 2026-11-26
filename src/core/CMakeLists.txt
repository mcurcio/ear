# ear_core: the desired graph and its edits, the node catalogue contract, the node framework, the
# reconciler, levels, the graph read and the executor contract (docs/architecture.md#packages).
# It never calls PipeWire, opens a socket, starts a thread or touches a file: nlohmann/json is its
# only dependency, and the engine and the control port reach it through the interfaces its
# headers declare.
add_library(ear_core STATIC
	claims.cpp
	command_error.cpp
	companions.cpp
	completions.cpp
	counter_watch.cpp
	cut_ring.cpp
	desired_graph.cpp
	graph_status.cpp
	latency.cpp
	levels.cpp
	log.cpp
	loop.cpp
	main_loop_activity.cpp
	node_catalogue.cpp
	node_host.cpp
	node_lifecycle.cpp
	note_gate.cpp
	pass_through.cpp
	realtime_channels.cpp
	program.cpp
	decimal_text.cpp
	rate_text.cpp
	ports.cpp
	readiness.cpp
	reconciler.cpp
	rtp_receiver.cpp
	settings_table.cpp
	sound_card.cpp
	recording.cpp
	source_ring.cpp
	status_book.cpp
	status_spec.cpp
	store.cpp)
# The public headers as a header set: its base directory is the include path of ear_core and
# of everything that links it, and VERIFY_INTERFACE_HEADER_SETS adds a target that compiles
# each header on its own, so none depends on what an includer happened to include first. The
# test executable depends on that target, so "make test" builds it.
target_sources(ear_core PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/core/action_spec.hpp
	include/ear/core/block_rules.hpp
	include/ear/core/claims.hpp
	include/ear/core/command_error.hpp
	include/ear/core/companions.hpp
	include/ear/core/counter_watch.hpp
	include/ear/core/cut_ring.hpp
	include/ear/core/cycle_counters.hpp
	include/ear/core/cycle_facts.hpp
	include/ear/core/desired_graph.hpp
	include/ear/core/executor.hpp
	include/ear/core/frame_ring.hpp
	include/ear/core/graph_spec.hpp
	include/ear/core/graph_view.hpp
	include/ear/core/internal_error.hpp
	include/ear/core/io_executor.hpp
	include/ear/core/latency.hpp
	include/ear/core/levels.hpp
	include/ear/core/lifecycle.hpp
	include/ear/core/log.hpp
	include/ear/core/loop.hpp
	include/ear/core/main_loop_activity.hpp
	include/ear/core/node_catalogue.hpp
	include/ear/core/node_handle.hpp
	include/ear/core/node_host.hpp
	include/ear/core/node_lifecycle.hpp
	include/ear/core/node_status.hpp
	include/ear/core/node_type.hpp
	include/ear/core/pass_through.hpp
	include/ear/core/program.hpp
	include/ear/core/decimal_text.hpp
	include/ear/core/rate_text.hpp
	include/ear/core/ready_events.hpp
	include/ear/core/realtime_channels.hpp
	include/ear/core/reconciler.hpp
	include/ear/core/recording.hpp
	include/ear/core/report.hpp
	include/ear/core/rtp_receiver.hpp
	include/ear/core/sample_rate.hpp
	include/ear/core/settings_table.hpp
	include/ear/core/sink.hpp
	include/ear/core/sink_ring.hpp
	include/ear/core/sound_card.hpp
	include/ear/core/source.hpp
	include/ear/core/source_ring.hpp
	include/ear/core/status_declarations.hpp
	include/ear/core/status_spec.hpp
	include/ear/core/store.hpp
	include/ear/core/store_rule.hpp
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
		include/ear/testing/erased.hpp
		include/ear/testing/harness_cuts.hpp
		include/ear/testing/node_harness.hpp
		include/ear/testing/report_on.hpp
		include/ear/testing/type_harness.hpp)
	set_target_properties(ear_type_harness PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
	# ear_sleeps: drive_now.hpp polls through its role call; and the raw sleeps poisoned here, as in every test program
	# (docs/testing.md#sleeps-by-role).
	target_link_libraries(ear_type_harness PUBLIC ear_core ear_sleeps)
	ear_poison_raw_sleeps(ear_type_harness "${EAR_NO_RAW_SLEEPS}" ear_sleeps)

	add_executable(ear_core_tests
		tests/actions_test.cpp
		tests/block_rules_test.cpp
		tests/claims_test.cpp
		tests/completions_test.cpp
		tests/counter_watch_test.cpp
		tests/cycle_counters_test.cpp
		tests/cycle_facts_test.cpp
		tests/desired_graph_test.cpp
		tests/frame_ring_test.cpp
		tests/graph_spec_test.cpp
		tests/graph_status_test.cpp
		tests/latency_test.cpp
		tests/levels_test.cpp
		tests/live_test.cpp
		tests/loop_test.cpp
		tests/main_loop_activity_test.cpp
		tests/module_run_test.cpp
		tests/node_handle_test.cpp
		tests/node_host_test.cpp
		tests/node_lifecycle_test.cpp
		tests/note_gate_test.cpp
		tests/ports_test.cpp
		tests/program_test.cpp
		tests/decimal_text_test.cpp
		tests/rate_text_test.cpp
		tests/readiness_test.cpp
		tests/realtime_test.cpp
		tests/report_test.cpp
		tests/reconfigure_test.cpp
		tests/reshape_test.cpp
		tests/reconciler_test.cpp
		tests/rtp_receiver_test.cpp
		tests/settings_table_test.cpp
		tests/sink_pause_test.cpp
		tests/sound_card_test.cpp
		tests/source_ring_test.cpp
		tests/status_spec_test.cpp
		tests/store_test.cpp
		tests/type_spec_test.cpp)
	target_link_libraries(ear_core_tests PRIVATE ear_core ear_type_harness ear_latency_table ear_cycle_table ear_inducers
		ear_allocation_count ear_catch_main)
	# The readiness list, the note gate, a configuration's completions and the claim registry are private to ear_core
	# (readiness.hpp, note_gate.hpp, completions.hpp, claims.hpp), and their rows test them directly.
	target_include_directories(ear_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
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
	ear_settings_table_compile_fail(rate-low-above-high "a rate's low is above its high")
	ear_settings_table_compile_fail(number-without-range "too few arguments")
	ear_settings_table_compile_fail(number-with-mustBe "too many arguments")
	ear_settings_table_compile_fail(default-outside-range "byDefault is outside its range")
	ear_settings_table_compile_fail(default-text-row "only a whole number, decimal, boolean or choice row takes a default")
	ear_settings_table_compile_fail(default-text-row-bool "only a whole number, decimal, boolean or choice row takes a default")
	ear_settings_table_compile_fail(default-text-row-whole "only a whole number, decimal, boolean or choice row takes a default")
	ear_settings_table_compile_fail(indexed-text-family "only a whole number, decimal, boolean or choice row takes a default")
	ear_settings_table_compile_fail(default-rate-row "only a whole number, decimal, boolean or choice row takes a default")
	ear_settings_table_compile_fail(decimal-low-above-high "a decimal's low is above its high")
	ear_settings_table_compile_fail(decimal-bound-not-finite "a decimal's bounds are finite numbers")
	ear_settings_table_compile_fail(decimal-default-outside "a decimal's byDefault is outside its range")
	ear_settings_table_compile_fail(log-taper-nonpositive "a log taper's low is above 0")
	ear_settings_table_compile_fail(db-taper-range "a dB taper reaches from -60 dB or lower")
	ear_settings_table_compile_fail(default-from-on-required "only an optional row takes defaultFrom")
	ear_settings_table_compile_fail(default-from-on-defaulted "only an optional row takes defaultFrom")
	ear_settings_table_compile_fail(text-hint-empty-value "a text setting's values are never empty")
	ear_settings_table_compile_fail(help-empty "a row's help is a line, never empty")
	ear_settings_table_compile_fail(default-on-required "no matching function for call to 'required")
	ear_settings_table_compile_fail(text-without-mustBe "needs what it must be")
	ear_settings_table_compile_fail(text-without-meaning "text<nullptr>")
	ear_settings_table_compile_fail(text-with-range "could not convert '1' from 'int' to 'std::string_view'")
	ear_settings_table_compile_fail(text-viewing-value "no matching function for call to 'text<")
	ear_settings_table_compile_fail(only-without-lower "must name a row above")
	ear_settings_table_compile_fail(raw-row "constraint failure")
	ear_settings_table_compile_fail(read-unlisted-row "no matching function for call to .*get<")
	ear_settings_table_compile_fail(read-number-as-text "conversion from")
	ear_settings_table_compile_fail(read-optional-unchecked "cannot convert")
	ear_settings_table_compile_fail(override-parse "parse.*marked 'override'")
	ear_settings_table_compile_fail(override-setting-kinds "settingKinds.*marked 'override'")
	ear_settings_table_compile_fail(override-name "name.*marked 'override'")
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
	ear_settings_table_compile_fail(two-marks "a row has one change mark")
	ear_settings_table_compile_fail(restarts-empty-reason "a restart states why")
	ear_settings_table_compile_fail(indexed-count-below "a family's count row is a row above it in its table")
	ear_settings_table_compile_fail(indexed-count-not-whole "a family's count row is a whole number")
	ear_settings_table_compile_fail(indexed-count-high-zero "a family's count row reaches 1 or more")
	ear_settings_table_compile_fail(indexed-count-optional "a family's count row always reads a value")
	ear_settings_table_compile_fail(indexed-collides-with-row "a row's key is a family member's spelling")
	ear_settings_table_compile_fail(read-family-without-member "no matching function for call to .*get<")
	ear_settings_table_compile_fail(read-member-of-plain-row "no matching function for call to .*get<")
	ear_settings_table_compile_fail(choice-empty "a choice names one value or more")
	ear_settings_table_compile_fail(choice-repeated-name "a choice names a value twice")
	ear_settings_table_compile_fail(choice-repeated-value "a choice names one enumerator twice")
	ear_settings_table_compile_fail(choice-unwritable-name "a choice's name is a word or a string ear.conf can hold")
	ear_settings_table_compile_fail(choice-too-many "a choice holds at most 32 names")
	ear_settings_table_compile_fail(choice-default-unnamed "a choice's byDefault is an enumerator it names")
	ear_settings_table_compile_fail(choice-not-enum "no matching function for call to 'choice<int>")

	# What must not compile about the latency model (docs/engine.md#latency-per-bank-pair): a delay is in series, never
	# a path of its own; tests/compile_fail/latency_bad.cpp says what each case is.
	set(bad_latency "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/latency_bad.cpp")
	ear_compile_check(ear_latency_compile_fail_control "${bad_latency}")
	function(ear_latency_compile_fail case regex)
		ear_compile_fail(ear_latency_compile_fail_${case} "${bad_latency}" ${case} "${regex}")
	endfunction()
	ear_latency_compile_fail(beyond-as-peer "a Delay lies in series")
	ear_latency_compile_fail(delay-as-path "a Delay lies in series")
	ear_latency_compile_fail(delay-as-latency "conversion from 'ear::Delay'")
	ear_latency_compile_fail(delay-as-optional-peer "no matching function for call to 'std::optional<ear::Latency>")
	ear_latency_compile_fail(paths-in-series "no match for 'operator\\+'")

	# What must not compile about a rate (docs/engine.md#dating): a raw number is never a rate, and a rate is never an
	# operand, so frames and time convert only through FrameClock; tests/compile_fail/sample_rate_bad.cpp says what
	# each case is.
	set(bad_rate "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/sample_rate_bad.cpp")
	ear_compile_check(ear_sample_rate_compile_fail_control "${bad_rate}")
	function(ear_sample_rate_compile_fail case regex)
		ear_compile_fail(ear_sample_rate_compile_fail_${case} "${bad_rate}" ${case} "${regex}")
	endfunction()
	ear_sample_rate_compile_fail(raw-integer-constructed "SampleRate::SampleRate\\(uint32_t\\)' is private")
	ear_sample_rate_compile_fail(raw-integer-converted "conversion from 'int' to non-scalar type 'const ear::SampleRate'")
	ear_sample_rate_compile_fail(raw-integer-passed "could not convert '48000' from 'int' to 'ear::SampleRate'")
	ear_sample_rate_compile_fail(fixed-zero "no matching function for call to 'ear::SampleRate::fixed<0>\\(\\)'")
	ear_sample_rate_compile_fail(rate-as-integer "cannot convert 'ear::SampleRate' to 'const uint32_t'")
	ear_sample_rate_compile_fail(rate-cast "invalid 'static_cast' from type 'ear::SampleRate'")
	ear_sample_rate_compile_fail(rate-times-frames "no match for 'operator\\*' \\(operand types are 'ear::SampleRate'")
	ear_sample_rate_compile_fail(frames-over-rate "no match for 'operator/' \\(operand types are [^)]* and 'ear::SampleRate'")
	ear_sample_rate_compile_fail(frames-remainder-rate "no match for 'operator%' \\(operand types are [^)]* and 'ear::SampleRate'")
	ear_sample_rate_compile_fail(rate-plus-frames "no match for 'operator\\+' \\(operand types are 'ear::SampleRate'")
	ear_sample_rate_compile_fail(rate-times-duration "no match for 'operator\\*' \\(operand types are 'ear::SampleRate' and 'std::chrono::")
	ear_sample_rate_compile_fail(duration-over-rate "no match for 'operator/' \\(operand types are 'std::chrono::nanoseconds'.* and 'ear::SampleRate'")
	ear_sample_rate_compile_fail(rate-compared-with-integer "no match for 'operator==' \\(operand types are 'ear::SampleRate' and 'int'\\)")
	ear_sample_rate_compile_fail(rate-as-frames "cannot convert 'ear::SampleRate' to 'int64_t'")
	ear_sample_rate_compile_fail(frame-clock-from-integer "use of deleted function 'ear::FrameClock::FrameClock\\(auto:[0-9]+\\) \\[with auto:[0-9]+ = int\\]'")
	ear_sample_rate_compile_fail(frame-clock-from-unsigned "use of deleted function 'ear::FrameClock::FrameClock\\(auto:[0-9]+\\) \\[with auto:[0-9]+ = unsigned int\\]'")
	ear_sample_rate_compile_fail(format-from-integer "could not convert '48000' from 'int' to 'ear::SampleRate'")
	ear_sample_rate_compile_fail(sink-ring-at-integer "no matching function for call to 'ear::SinkRing::SinkRing\\(int, int, int\\)'")
	ear_sample_rate_compile_fail(level-window-at-integer "could not convert '48000' from 'int' to 'ear::SampleRate'")

	# What must not compile about a type's status fields, events and log codes
	# (docs/writing-a-node-type.md#status-and-events): tests/compile_fail/status_spec_bad.cpp says what each case is.
	set(bad_status "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/status_spec_bad.cpp")
	ear_compile_check(ear_status_spec_compile_fail_control "${bad_status}")
	function(ear_status_spec_compile_fail case regex)
		ear_compile_fail(ear_status_spec_compile_fail_${case} "${bad_status}" ${case} "${regex}")
	endfunction()
	ear_status_spec_compile_fail(rt-text-field "a text field is never written from the realtime thread")
	ear_status_spec_compile_fail(writer-loop-not-in-spec "a writer::loop status field names one of its spec's loops")
	ear_status_spec_compile_fail(choice-value-misspelt "not one of the choice field's values")
	ear_status_spec_compile_fail(initially-not-a-value "initial value is outside its range")
	ear_status_spec_compile_fail(sentence-unknown-place "a sentence's place names no argument")
	ear_status_spec_compile_fail(sentence-missing-arg "an argument has no place in the sentence")
	ear_status_spec_compile_fail(duplicate-status-name "a type's status fields have unique names")
	ear_status_spec_compile_fail(duplicate-event-code "a type's events have unique codes")
	ear_status_spec_compile_fail(log-code-text-arg "the realtime thread writes no text")
	ear_status_spec_compile_fail(log-code-five-args "a log code has at most four arguments")
	ear_status_spec_compile_fail(status-in-unnamed-namespace "status fields are declared in a named namespace")
	ear_status_spec_compile_fail(event-in-unnamed-namespace "events are declared in a named namespace")

	# What must not compile about what a code writes on the realtime thread through Io, and what Report takes
	# (docs/writing-a-node-type.md#reporting-from-the-code): tests/compile_fail/realtime_bad.cpp says what each case is.
	set(bad_realtime "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/realtime_bad.cpp")
	ear_compile_check(ear_realtime_compile_fail_control "${bad_realtime}")
	function(ear_realtime_compile_fail case regex)
		ear_compile_fail(ear_realtime_compile_fail_${case} "${bad_realtime}" ${case} "${regex}")
	endfunction()
	ear_realtime_compile_fail(set-field-not-in-spec "io.set takes only its spec's status fields")
	ear_realtime_compile_fail(set-field-not-rt "io.set takes only a writer::rt field")
	ear_realtime_compile_fail(raise-text-arg-from-process "an event with a text argument is never raised from process")
	ear_realtime_compile_fail(raise-wrong-arg-count "io.raise takes one value per argument of the event")
	ear_realtime_compile_fail(raise-event-not-in-spec "io.raise takes only its spec's events")
	ear_realtime_compile_fail(raise-choice-by-text "a choice argument from process is a value made at compile time")
	ear_realtime_compile_fail(log-code-not-in-spec "io.log takes only its spec's log codes")
	ear_realtime_compile_fail(choice-arg-misspelt "not one of the choice argument's values")
	ear_realtime_compile_fail(report-set-rt-field "a writer::rt field is written by the node's code, through Io")
	ear_realtime_compile_fail(report-of-other-spec "set takes only its spec's status fields")
	ear_realtime_compile_fail(report-constructed "Report<.*>::Report\\(.*is private within this context")
	ear_realtime_compile_fail(pass-through-with-rt "answers with a code of its own, never a pass-through")

	# Live rows (docs/writing-a-node-type.md#live-rows): their marks, their reads and io.param;
	# tests/compile_fail/live_bad.cpp says what each case is.
	set(bad_live "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/live_bad.cpp")
	ear_compile_check(ear_live_compile_fail_control "${bad_live}")
	function(ear_live_compile_fail case regex)
		ear_compile_fail(ear_live_compile_fail_${case} "${bad_live}" ${case} "${regex}")
	endfunction()
	ear_live_compile_fail(live-text-row "a live row is a decimal, whole number, boolean or choice")
	ear_live_compile_fail(live-rate-row "a live row is a decimal, whole number, boolean or choice")
	ear_live_compile_fail(live-optional-row "a live row always reads a value")
	ear_live_compile_fail(ramp-whole-number "a ramp needs a decimal or a boolean row")
	ear_live_compile_fail(ramp-choice "a ramp needs a decimal or a boolean row")
	ear_live_compile_fail(ramp-zero "a ramp takes 1 to 1000 ms")
	ear_live_compile_fail(live-shapes-ports "a live row never shapes the ports")
	ear_live_compile_fail(live-and-in-place "a row has one change mark")
	ear_live_compile_fail(live-members-over-256 "a type has at most 256 live members")
	ear_live_compile_fail(get-live-row "a live row is read through io.param, or liveValue")
	ear_live_compile_fail(count-live-row "a family whose count row is live is counted by liveCount")
	ear_live_compile_fail(live-value-not-live "liveValue reads a live row")
	ear_live_compile_fail(param-not-live "io.param reads a live row")
	ear_live_compile_fail(param-row-not-in-spec "io.param takes only its spec's rows")
	ear_live_compile_fail(param-family-without-member "a family is read by member")
	ear_live_compile_fail(param-member-of-plain-row "only a family is read by member")

	# A type's actions (docs/writing-a-node-type.md#actions): tests/compile_fail/action_bad.cpp says what each case is.
	set(bad_action "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/action_bad.cpp")
	ear_compile_check(ear_action_compile_fail_control "${bad_action}")
	function(ear_action_compile_fail case regex)
		ear_compile_fail(ear_action_compile_fail_${case} "${bad_action}" ${case} "${regex}")
	endfunction()
	ear_action_compile_fail(action-named-lifecycle "an action is never named start, stop, restart, disable or enable")
	ear_action_compile_fail(action-name-not-kebab "an action's name is kebab-case")
	ear_action_compile_fail(call-loop-not-in-spec "a call names one of its spec's loops")
	ear_action_compile_fail(action-arg-marked "a call's arguments never change, so they carry no change mark")
	ear_action_compile_fail(action-arg-indexed "a call's arguments are a fixed set of keys: no indexed family")
	ear_action_compile_fail(call-handler-signature "a call's handler is std::expected<void, std::string> act")
	ear_action_compile_fail(triggered-not-a-trigger "io.triggered takes a trigger")
	ear_action_compile_fail(pass-through-with-trigger "answers with a code of its own, never a pass-through")

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
	ear_type_spec_compile_fail(pass-through-no-pair "a pass-through names at least one pair of banks")
	ear_type_spec_compile_fail(code-given-as-pointer "configure answers with its code by value")
	ear_type_spec_compile_fail(pass-through-pairs-made-by-hand "PassThroughPairs::PassThroughPairs\\(.*is private within this context")
	# The erased layer's answer is not a type's to make (docs/writing-a-node-type.md#configure-and-createof).
	ear_type_spec_compile_fail(answer-through-configured "Configured::erased\\(.*is private within this context")
	ear_type_spec_compile_fail(answer-through-the-erased-instance "ErasedInstance::applied\\(.*is protected within this context")
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
	# The change version (the control writes one; docs/writing-a-node-type.md#the-change-version): of the wrong shape,
	# not; its context offers only a change's calls, and creation's none of them.
	ear_type_spec_compile_fail(change-version-wrong-signature "a type's change version is Changed configure")
	ear_type_spec_compile_fail(change-version-wrong-context "a type's change version is Changed configure")
	ear_type_spec_compile_fail(change-context-has-no-state "has no member named 'state'")
	ear_type_spec_compile_fail(hold-names-an-input-bank "hold names a routable output bank of its spec")
	ear_type_spec_compile_fail(change-answered-applied-without-context "'applied' is not a member of 'ear::Changed'")
	ear_type_spec_compile_fail(create-context-has-no-hold "has no member named 'hold'")
	ear_type_spec_compile_fail(reconfigure-deadline-below-1s "a type's reconfigureDeadline is 1 s to 60 s")
	ear_type_spec_compile_fail(reconfigure-deadline-above-60s "a type's reconfigureDeadline is 1 s to 60 s")
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
	ear_type_spec_compile_fail(rtp-receiver-on-routable-bank "an RTP receiver joins an inside input bank of the spec")
	ear_type_spec_compile_fail(sink-on-input-bank "a sink reads an inside output bank of the spec")
	ear_type_spec_compile_fail(sink-on-routable-bank "a sink reads an inside output bank of the spec")
	ear_type_spec_compile_fail(sink-without-consumer "a sink is given its one consumer")
	ear_type_spec_compile_fail(source-on-output-bank "a source plays into an inside input bank of the spec")
	ear_type_spec_compile_fail(source-on-routable-bank "a source plays into an inside input bank of the spec")
	ear_type_spec_compile_fail(declare-latency-bank-the-spec-lacks "declareLatency takes its spec's banks")
	ear_type_spec_compile_fail(declare-latency-backwards "declareLatency names a pair: an input bank, then an output bank")
	ear_type_spec_compile_fail(context-bank-the-spec-lacks "a context takes only its spec's banks")
	ear_type_spec_compile_fail(engine-companion-bank-the-spec-lacks "a context takes only its spec's banks")
	ear_type_spec_compile_fail(program-path-relative "a program's executable is an absolute path")
	ear_type_spec_compile_fail(program-path-not-plain "a program's executable is an absolute path of plain components")
	ear_type_spec_compile_fail(pipe-receiver-on-output-bank "a pipe receiver joins an inside input bank of the spec")
	ear_type_spec_compile_fail(pipe-receiver-on-routable-bank "a pipe receiver joins an inside input bank of the spec")
	ear_type_spec_compile_fail(attach-no-companion-shape "attach takes a companion's shape")
	ear_type_spec_compile_fail(fifo-made-by-hand "Fifo::Fifo\\(.*is private within this context")
	ear_type_spec_compile_fail(program-feed-stateless-loop "a program's feed reaches a loop declared with a state")
	ear_type_spec_compile_fail(program-feed-loop-the-spec-lacks "a context takes only its spec's loops")
	# A sink's consumer named in a loop's state (docs/writing-a-node-type.md#sinks): a loop of the spec, declared with
	# a state, and a member of that state (or the state itself) that is a SinkConsumer.
	ear_type_spec_compile_fail(sink-loop-the-spec-lacks "a sink is consumed by one of its spec's loops")
	ear_type_spec_compile_fail(sink-member-of-another-state "a sink's consumer is a member of that loop's state")
	ear_type_spec_compile_fail(sink-member-for-stateless-loop "a sink's consumer is a member of that loop's state")
	ear_type_spec_compile_fail(sink-member-not-a-consumer "a sink's consumer is a SinkConsumer")
	ear_type_spec_compile_fail(sink-state-not-a-consumer "a loop's state that consumes a sink is a SinkConsumer")
	# Loops (docs/writing-a-node-type.md#loops): a loop named wrongly or twice, a state where none is declared, a loop the
	# spec lacks, and a post of a function of another loop's state.
	ear_type_spec_compile_fail(loop-name-invalid "a loop's name is 1 to 8 of")
	ear_type_spec_compile_fail(loops-named-twice "a type's loops have unique names")
	ear_type_spec_compile_fail(state-to-stateless-loop "a loop declared without a state takes none")
	ear_type_spec_compile_fail(state-missing "configure's answer gives the state of every loop its spec declares with one")
	ear_type_spec_compile_fail(state-given-twice "configure's answer gives a loop's state once")
	ear_type_spec_compile_fail(state-loop-the-spec-lacks "a context takes only its spec's loops")
	ear_type_spec_compile_fail(state-made-by-hand "LoopState<.*>::LoopState\\(.*is private within this context")
	ear_type_spec_compile_fail(state-given-as-lvalue "use of deleted function .*LoopState")
	ear_type_spec_compile_fail(answer-given-a-state-by-pointer "configure's answer is its code, then its loops' states")
	ear_type_spec_compile_fail(code-of-another-spec "configure answers with a code written against its own type's spec")
	ear_type_spec_compile_fail(executor-loop-the-spec-lacks "a context takes only its spec's loops")
	ear_type_spec_compile_fail(post-loop-the-spec-lacks "a post names one of its spec's loops")
	ear_type_spec_compile_fail(post-other-loop-state "a post to a loop takes a function of that loop's state")
	ear_type_spec_compile_fail(harness-loop-the-spec-lacks "the harness takes only its spec's loops")
	ear_type_spec_compile_fail(row-unmarked "every settings row of a spec has one change mark")
	# Stores (docs/writing-a-node-type.md#stores): a store's rule broken (no maxBytes, more than 1 MiB, a mode beyond
	# 0777), a suffix named wrongly or twice, a store the spec lacks (in a context or the harness), and the spec's members
	# out of order (stores come before loops).
	ear_type_spec_compile_fail(store-suffix-invalid "a store's suffix is 1 to 16 of")
	ear_type_spec_compile_fail(stores-named-twice "a type's stores have unique suffixes")
	ear_type_spec_compile_fail(store-max-bytes-left-out "a_store_gives_its_maxBytes")
	ear_type_spec_compile_fail(store-above-1-mib "a_store_holds_at_most_1_MiB")
	ear_type_spec_compile_fail(store-mode-beyond-0777 "a_store_mode_has_no_bits_outside_0777")
	ear_type_spec_compile_fail(store-the-spec-lacks "a context takes only its spec's stores")
	ear_type_spec_compile_fail(harness-store-the-spec-lacks "the harness takes only its spec's stores")
	ear_type_spec_compile_fail(stores-after-loops "designator order")
endif()
