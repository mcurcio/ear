# ear_catch_main: the Catch2 runner (vendored single header, main.cpp) as a library. Every test
# executable links it and gets Catch's header with it, then registers its test cases with
# ear_catch_tests(<target>) (one CTest test per Catch test case).
add_library(ear_catch_main STATIC main.cpp)
target_include_directories(ear_catch_main SYSTEM PUBLIC
	"${PROJECT_SOURCE_DIR}/vendor/catch/single_include")

# Sleeps by role (docs/testing.md#sleeps-by-role): ear_sleeps is the role calls a test sleeps through
# (support/sleeps.hpp), and every target that compiles test code is compiled with support/no_raw_sleeps.hpp forced in
# (ear_poison_raw_sleeps), which poisons every raw sleep and Catch's WARN and CHECK_NOFAIL after them. ear_catch_tests
# gives it to each test program, src/core to ear_type_harness; configure refuses any other target with a source in a
# test, tests or testing folder that lacks it. Catch's runner is the one exempt: it includes Catch's implementation
# behind its own configuration, which a forced Catch header would come before, and holds no test.
add_library(ear_sleeps INTERFACE)
target_include_directories(ear_sleeps INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
include("${PROJECT_SOURCE_DIR}/cmake/RawSleeps.cmake")
set(EAR_NO_RAW_SLEEPS "${CMAKE_CURRENT_SOURCE_DIR}/support/no_raw_sleeps.hpp" CACHE INTERNAL
	"The header forced into every test target (read from sibling directories)")
ear_refuse_raw_sleeps("${EAR_NO_RAW_SLEEPS}" EXEMPT ear_catch_main)

# ear_catch_tests(<target>): the raw sleeps poisoned in every source of <target> (above), and catch_discover_tests
# with --warn NoTests, so a CTest test whose name
# matches no Catch test case fails (exit 2) instead of passing having run nothing. A Catch name
# holding ';' is split by CMake into two such tests, and Catch's own "No test cases matched"
# exits 0; this is what makes that, or any other filter that runs nothing, fail. Catch.cmake is
# included inside the function: it sets the path of its discovery script as a variable, which a
# sibling directory (src/<package>) would not see from here.
#
# SERIAL <test spec>: the test cases the Catch test spec matches (a tag, such as [s2]) are discovered apart, with the
# CTest property RUN_SERIAL, so ctest --parallel runs nothing beside them: for a test whose load would starve the
# realtime-timed tests running beside it (PipeWire's data thread keeping time), and theirs it.
#
# Each executable's build also fails on its own, before and after discovery, since catch_discover_tests takes the
# listing's exit code as a count of tests (only a negative one is fatal) and a listing that ran nothing would leave the
# executable out of ctest unseen: first the executable checks its test names (test/main.cpp, --ear-check-test-names),
# then catch_tests_guard.cmake fails unless each discovered file names at least one test.
set(EAR_CATCH_TESTS_GUARD "${CMAKE_CURRENT_SOURCE_DIR}/catch_tests_guard.cmake" CACHE INTERNAL
	"ear_catch_tests' check that discovery found tests (read from sibling directories)")
function(ear_catch_tests target)
	cmake_parse_arguments(PARSE_ARGV 1 arg "" "SERIAL" "")
	if(arg_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "ear_catch_tests: unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
	endif()
	include(Catch)
	ear_poison_raw_sleeps(${target} "${EAR_NO_RAW_SLEEPS}" ear_sleeps)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND $<TARGET_FILE:${target}> --ear-check-test-names
		COMMENT "Checking ${target}'s test names"
		VERBATIM)
	set(extra --warn NoTests)
	if(arg_SERIAL)
		catch_discover_tests(${target} TEST_SPEC "~${arg_SERIAL}" EXTRA_ARGS ${extra})
		catch_discover_tests(${target} TEST_SPEC "${arg_SERIAL}" EXTRA_ARGS ${extra} PROPERTIES RUN_SERIAL TRUE)
		set(specs "~${arg_SERIAL}" "${arg_SERIAL}")
	else()
		catch_discover_tests(${target} EXTRA_ARGS ${extra})
		set(specs "")
	endif()
	# The file each catch_discover_tests call writes, named as Catch.cmake names it.
	set(files "")
	foreach(spec IN LISTS specs)
		string(SHA1 hash "${spec} ${extra}")
		string(SUBSTRING ${hash} 0 7 hash)
		list(APPEND files "${CMAKE_CURRENT_BINARY_DIR}/${target}_tests-${hash}.cmake")
	endforeach()
	if(NOT specs)
		string(SHA1 hash " ${extra}")
		string(SUBSTRING ${hash} 0 7 hash)
		set(files "${CMAKE_CURRENT_BINARY_DIR}/${target}_tests-${hash}.cmake")
	endif()
	string(REPLACE ";" "|" files_arg "${files}") # a ';' would split the argument
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND "${CMAKE_COMMAND}" "-DEXECUTABLE=${target}" "-DFILES=${files_arg}"
			-P "${EAR_CATCH_TESTS_GUARD}"
		VERBATIM)
endfunction()

# ear_compile_check(<name> <source> [<flag>...]): compiles <source> (syntax only) against ear_core's headers as a test.
function(ear_compile_check name source)
	add_test(NAME ${name}
		COMMAND "${CMAKE_CXX_COMPILER}" ${CMAKE_CXX23_STANDARD_COMPILE_OPTION} -w -fsyntax-only
			"-I$<JOIN:$<TARGET_PROPERTY:ear_core,INTERFACE_INCLUDE_DIRECTORIES>,;-I>" ${ARGN} "${source}"
		COMMAND_EXPAND_LISTS)
endfunction()

# ear_compile_fail(<name> <source> <case> <regex>): <source> with EAR_CASE_<case> defined must be refused for the reason
# <regex> names. A case that compiles prints nothing and fails, and so does a wrong reason; a broken include is what
# the plain ear_compile_check of the same source catches. A '-' in <case> is '_' in the macro.
function(ear_compile_fail name source case regex)
	string(MAKE_C_IDENTIFIER "${case}" macro)
	ear_compile_check(${name} "${source}" -DEAR_CASE_${macro})
	set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

# The ear binary starts, loads its shared libraries and prints its options.
add_test(NAME ear_help COMMAND ear --help)
add_test(NAME ear_help_lists_config COMMAND ear --help)
# The graph file folder and the engine's fallback format, in the order --help lists them.
set_tests_properties(ear_help_lists_config PROPERTIES
	PASS_REGULAR_EXPRESSION "--config-dir.*--rate.*--block-size")

# What configure asks of a tree that must carry a patch (cmake/SendspinDeps.cmake).
add_test(NAME ear_carried_patch_check COMMAND "${CMAKE_COMMAND}"
	"-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/carried_patch_test" -P "${CMAKE_CURRENT_SOURCE_DIR}/carried_patch_test.cmake")

# The worked example of docs/writing-a-node-type.md, the `tone` type, taken from the page and compiled (syntax only)
# against today's headers, so the page cannot drift from them: the type, with the folder's name its table must carry,
# and its tests (with the raw sleeps poisoned, as every test program). It is never a node type of this build: nothing
# under src/nodes/ is made.
set(doc_tone "${CMAKE_CURRENT_BINARY_DIR}/doc_example/tone")
add_test(NAME ear_doc_example_extract COMMAND "${CMAKE_COMMAND}"
	"-DDOC=${PROJECT_SOURCE_DIR}/docs/writing-a-node-type.md" "-DOUT=${doc_tone}"
	-P "${CMAKE_CURRENT_SOURCE_DIR}/doc_example.cmake")
set_tests_properties(ear_doc_example_extract PROPERTIES FIXTURES_SETUP doc_example)
ear_compile_check(ear_doc_example_type "${doc_tone}/tone.cpp" "-I${doc_tone}" "-I${doc_tone}/include"
	"-DEAR_NODE_TYPE_NAME=\"tone\"")
ear_compile_check(ear_doc_example_tests "${doc_tone}/tests/tone_test.cpp" "-I${doc_tone}" "-I${doc_tone}/include"
	"-I${PROJECT_SOURCE_DIR}/vendor/catch/single_include" -include "${EAR_NO_RAW_SLEEPS}")
set_tests_properties(ear_doc_example_type ear_doc_example_tests PROPERTIES FIXTURES_REQUIRED doc_example)

# ear_ws_client: the tests' WebSocket client (test/support/ws_client.hpp, header-only, on
# Boost.Beast), for the control port's tests (src/server/tests) and the system tests.
add_library(ear_ws_client INTERFACE)
target_include_directories(ear_ws_client INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(ear_ws_client INTERFACE Boost::beast)

# ear_temp_folder: the tests' temporary folder (test/support/temp_folder.hpp, header-only), for every test
# program that makes folders on disk, and WithoutFilePrivileges (without_file_privileges.hpp), for a case that needs a
# folder it cannot write under root too (docs/testing.md#an-ordinary-users-folder).
add_library(ear_temp_folder INTERFACE)
target_include_directories(ear_temp_folder INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")

# ear_rtp_tone: the tests' RTP sender and tone fit (test/support/rtp_tone.hpp, header-only), for
# the RTP receiver's engine tests (src/pipewire/tests) and the system tests (docs/node-types.md#rtp).
# ear_core for FrameClock, through which a late send's reach is the receiver's configured buffer.
add_library(ear_rtp_tone INTERFACE)
target_include_directories(ear_rtp_tone INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(ear_rtp_tone INTERFACE ear_core ear_sleeps)

# ear_sendspin_server: the real Sendspin server for the sendspin node's clock and system tests (test/support/
# sendspin_server.py, run by sendspin_server.hpp, header-only, in the venv CMakeLists.txt checks for).
add_library(ear_sendspin_server INTERFACE)
target_include_directories(ear_sendspin_server INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_compile_definitions(ear_sendspin_server INTERFACE
	EAR_SENDSPIN_SERVER_PYTHON="${EAR_SENDSPIN_SERVER_PYTHON}"
	EAR_SENDSPIN_SERVER_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/support/sendspin_server.py")

# ear_standard_error_lines: what a library in the test's process prints, read as events (test/support/
# standard_error_lines.hpp, header-only), for the sendspin clock tests (docs/testing.md#the-librarys-own-word).
add_library(ear_standard_error_lines INTERFACE)
target_include_directories(ear_standard_error_lines INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")

# ear_node_types_tests: what holds for every node type together (each type's own tests are in src/nodes/<type>).
add_subdirectory(nodes)

# ear_inducers: the rare events a test makes itself (test/support/inducers.hpp, header-only): planned data-thread stalls
# and the cycle ledger that judges over clean cycles (docs/testing.md#making-the-rare-event). ear_statistics: a
# statistical bound from the count measured (test/support/statistics.hpp, docs/testing.md#statistics-from-the-count).
add_library(ear_inducers INTERFACE)
target_include_directories(ear_inducers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(ear_inducers INTERFACE ear_core ear_sleeps)
add_library(ear_statistics INTERFACE)
target_include_directories(ear_statistics INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")

# ear_latency_table: the latency model's table of bank shapes (test/support/latency_table.hpp, header-only), run
# through workOut by ear_core_tests and through a real port block by ear_pipewire_tests
# (docs/testing.md#what-each-test-program-proves).
add_library(ear_latency_table INTERFACE)
target_include_directories(ear_latency_table INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(ear_latency_table INTERFACE ear_core)

# ear_cycle_table: the cycle facts' table of scripted cycles (test/support/cycle_table.hpp, header-only), run through
# CycleAccount and the type harness by ear_core_tests and through a real port block by ear_pipewire_tests
# (docs/testing.md#what-each-test-program-proves).
add_library(ear_cycle_table INTERFACE)
target_include_directories(ear_cycle_table INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(ear_cycle_table INTERFACE ear_core)

# ear_cycle_judge: the one judge of lost cycles (test/support/cycle_judge.hpp, header-only), the only place a test
# excuses a cycle its judged node missed (docs/testing.md#the-cycle-judge).
add_library(ear_cycle_judge INTERFACE)
target_include_directories(ear_cycle_judge INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(ear_cycle_judge INTERFACE ear_core ear_inducers ear_cycle_table)

# ear_test_support_tests: the tools themselves, on cycles and samples made by hand.
add_executable(ear_test_support_tests support/tests/inducers_test.cpp support/tests/cycle_judge_test.cpp
	support/tests/statistics_test.cpp support/tests/without_file_privileges_test.cpp)
target_link_libraries(ear_test_support_tests PRIVATE ear_inducers ear_cycle_judge ear_statistics ear_temp_folder
	ear_catch_main)
ear_catch_tests(ear_test_support_tests)

# What must not compile in a test (docs/testing.md#sleeps-by-role): each raw way to sleep, and Catch's macros that tell
# and go on, refused by the poison no_raw_sleeps.hpp forces in, as every test target's sources are compiled; a poll or
# select on nothing; a role call that says no why. compile_fail/no_raw_sleeps_bad.cpp says what each case is, and its
# control (no case) must compile. The regexes are GCC's.
set(bad_sleeps "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/no_raw_sleeps_bad.cpp")
set(sleeps_flags "-I${PROJECT_SOURCE_DIR}/vendor/catch/single_include" "-I${CMAKE_CURRENT_SOURCE_DIR}/support"
	-include "${EAR_NO_RAW_SLEEPS}")
ear_compile_check(ear_no_raw_sleeps_compile_fail_control "${bad_sleeps}" ${sleeps_flags})
function(ear_no_raw_sleeps_compile_fail case regex)
	string(MAKE_C_IDENTIFIER "${case}" macro)
	set(test ear_no_raw_sleeps_compile_fail_${case})
	ear_compile_check(${test} "${bad_sleeps}" ${sleeps_flags} -DEAR_CASE_${macro})
	set_tests_properties(${test} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()
foreach(pair sleep-for:sleep_for sleep-until:sleep_until split-call:sleep_for using:sleep_for pasted:sleep_for
		macro-after:usleep usleep:usleep nanosleep:nanosleep clock-nanosleep:clock_nanosleep posix-sleep:sleep
		bare-sleep:sleep later-header:sleep_for warn:WARN catch-warn:CATCH_WARN check-nofail:CHECK_NOFAIL
		catch-check-nofail:CATCH_CHECK_NOFAIL undef-warn:WARN)
	string(REPLACE ":" ";" pair "${pair}")
	list(GET pair 0 case)
	list(GET pair 1 name)
	ear_no_raw_sleeps_compile_fail(${case} "attempt to use poisoned \"${name}\"")
endforeach()
foreach(call poll ppoll select pselect)
	ear_no_raw_sleeps_compile_fail(${call}-nothing "use of deleted function 'int ${call}\\(")
endforeach()
ear_no_raw_sleeps_compile_fail(empty-why "a_sleep_says_why_it_sleeps")
ear_no_raw_sleeps_compile_fail(why-not-a-literal "no matching function for call to 'induce")
ear_no_raw_sleeps_compile_fail(no-why "no matching function for call to 'negative")

# Configure stops at a target that compiles test code without the poison forced in, wherever its test source is, and
# goes on for a poisoned target, an exempt one, a target of product code alone and one whose only test-named source is
# generated in the build tree: sleep_refusal is a project of its own, because the refusal stops the whole configure.
function(ear_raw_sleeps_test name shape)
	add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/sleep_refusal"
		-B "${CMAKE_CURRENT_BINARY_DIR}/${name}" "-DEAR_CMAKE_DIR=${PROJECT_SOURCE_DIR}/cmake" -DEAR_SHAPE=${shape}
		"-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endfunction()
foreach(shape tests testing test mixed relative)
	ear_raw_sleeps_test(ear_raw_sleeps_refused_${shape} ${shape})
	set_tests_properties(ear_raw_sleeps_refused_${shape} PROPERTIES
		PASS_REGULAR_EXPRESSION "RawSleeps\\.cmake:[0-9]+ \\(message\\):[^a-z]*ear_user compiles test code \\([^)]*\\) without")
endforeach()
foreach(shape none poisoned exempt generated)
	ear_raw_sleeps_test(ear_raw_sleeps_allowed_${shape} ${shape})
endforeach()

# The tests' Python has no compiler to poison a name, so its rule is read from its syntax tree: python_sleeps.py refuses
# `sleep` named, imported or called anywhere in test/'s Python but in the body of a module-level function named for a
# role (induce, negative, poll, pace), as sendspin_server.py's are; and its own tests (python_sleeps/) refuse each way of
# naming it, let strings, comments and library calls that only contain the word through, and refuse a file that does
# not parse and a check of nothing.
set(ear_python_sleeps "${CMAKE_CURRENT_SOURCE_DIR}/python_sleeps.py")
set(ear_python_snippets "${CMAKE_CURRENT_SOURCE_DIR}/python_sleeps")
add_test(NAME ear_python_sleeps_by_role
	COMMAND "${EAR_SENDSPIN_SERVER_PYTHON}" -I "${ear_python_sleeps}" "${CMAKE_CURRENT_SOURCE_DIR}" "*.py")
set_tests_properties(ear_python_sleeps_by_role PROPERTIES
	PASS_REGULAR_EXPRESSION "python_sleeps: [1-9][0-9]* files under .*, every sleep in a role")
foreach(way time_sleep asyncio_sleep from_import aliased outside_role method_named_role nested_in_other)
	add_test(NAME ear_python_sleeps_refuses_${way}
		COMMAND "${EAR_SENDSPIN_SERVER_PYTHON}" -I "${ear_python_sleeps}" "${ear_python_snippets}" "${way}.py.snippet")
	set_tests_properties(ear_python_sleeps_refuses_${way} PROPERTIES
		PASS_REGULAR_EXPRESSION "${way}\\.py\\.snippet:[0-9]+: a sleep outside a role")
endforeach()
add_test(NAME ear_python_sleeps_allows_roles_and_lookalikes
	COMMAND "${EAR_SENDSPIN_SERVER_PYTHON}" -I "${ear_python_sleeps}" "${ear_python_snippets}" "allowed.py.snippet")
set_tests_properties(ear_python_sleeps_allows_roles_and_lookalikes PROPERTIES
	PASS_REGULAR_EXPRESSION "python_sleeps: 1 files under .*, every sleep in a role")
add_test(NAME ear_python_sleeps_refuses_what_does_not_parse
	COMMAND "${EAR_SENDSPIN_SERVER_PYTHON}" -I "${ear_python_sleeps}" "${ear_python_snippets}" "broken.py.snippet")
set_tests_properties(ear_python_sleeps_refuses_what_does_not_parse PROPERTIES
	PASS_REGULAR_EXPRESSION "broken\\.py\\.snippet: does not parse")
add_test(NAME ear_python_sleeps_refuses_checking_nothing
	COMMAND "${EAR_SENDSPIN_SERVER_PYTHON}" -I "${ear_python_sleeps}" "${ear_python_snippets}" "no-such-file.py")
set_tests_properties(ear_python_sleeps_refuses_checking_nothing PROPERTIES
	PASS_REGULAR_EXPRESSION "the check would check nothing")

# Where a rate leaves its type (docs/engine.md#dating): SampleRate has no arithmetic, so the compiler refuses a rate as
# an operand (src/core/tests/compile_fail/sample_rate_bad.cpp). What is left is one word: `hz`, a rate's number,
# appears in src/ outside tests only in the edges sample_rate_exits.cmake lists; and the check itself refusing each way
# of taking it (sample_rate_exits/<way>.snippet: a call, a member pointer, a name of its own), letting lookalikes
# through (allowed.snippet: khz, hzFrames, "Hz"), taking a listed file and skipping tests (listed/), refusing a listed
# file that is not there, and refusing to check nothing.
set(ear_rate_exits "${CMAKE_CURRENT_SOURCE_DIR}/sample_rate_exits.cmake")
set(ear_rate_snippets "${CMAKE_CURRENT_SOURCE_DIR}/sample_rate_exits")
add_test(NAME ear_sample_rate_exits
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${PROJECT_SOURCE_DIR}/src" -P "${ear_rate_exits}")
set_tests_properties(ear_sample_rate_exits PROPERTIES
	PASS_REGULAR_EXPRESSION "sample_rate_exits: [0-9]+ files under .*, 10 listed, none takes a rate's number elsewhere")
foreach(way call member_pointer own_name)
	add_test(NAME ear_sample_rate_exits_refuses_${way}
		COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_rate_snippets}" "-DGLOB=${way}.snippet" "-DALLOWED="
			-P "${ear_rate_exits}")
	set_tests_properties(ear_sample_rate_exits_refuses_${way} PROPERTIES
		PASS_REGULAR_EXPRESSION "${way}\\.snippet:1: a rate's number taken outside the listed edges")
endforeach()
add_test(NAME ear_sample_rate_exits_allows_lookalikes
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_rate_snippets}" "-DGLOB=allowed.snippet" "-DALLOWED="
		-P "${ear_rate_exits}")
set_tests_properties(ear_sample_rate_exits_allows_lookalikes PROPERTIES
	PASS_REGULAR_EXPRESSION "sample_rate_exits: 1 files under .*, 0 listed, none takes a rate's number elsewhere")
add_test(NAME ear_sample_rate_exits_takes_listed_and_skips_tests
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_rate_snippets}/listed" "-DGLOB=*.snippet" "-DALLOWED=edge.snippet"
		-P "${ear_rate_exits}")
set_tests_properties(ear_sample_rate_exits_takes_listed_and_skips_tests PROPERTIES
	PASS_REGULAR_EXPRESSION "sample_rate_exits: 2 files under .*, 1 listed, none takes a rate's number elsewhere")
add_test(NAME ear_sample_rate_exits_refuses_a_listed_file_not_there
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_rate_snippets}" "-DGLOB=allowed.snippet" "-DALLOWED=listed/gone.snippet"
		-P "${ear_rate_exits}")
set_tests_properties(ear_sample_rate_exits_refuses_a_listed_file_not_there PROPERTIES
	PASS_REGULAR_EXPRESSION "listed/gone\\.snippet is listed but not found")
add_test(NAME ear_sample_rate_exits_refuses_checking_nothing
	COMMAND "${CMAKE_COMMAND}" "-DROOT=${ear_rate_snippets}" "-DGLOB=no-such-file.snippet" "-DALLOWED="
		-P "${ear_rate_exits}")
set_tests_properties(ear_sample_rate_exits_refuses_checking_nothing PROPERTIES
	PASS_REGULAR_EXPRESSION "the check would check nothing")

# The guard of the engine's callback boundary (docs/architecture.md#the-callback-boundary), and its own tests.
add_subdirectory(pipewire)
