# catch_tests_guard.cmake -DEXECUTABLE=<name> -DFILES=<file>|<file>...: run after catch_discover_tests on each test
# executable's build (test/CMakeLists.txt, ear_catch_tests). Fails unless every file it wrote names at least one test:
# Catch.cmake takes a listing's exit code as a count of tests and goes on past one that listed nothing, which would
# leave the whole executable out of ctest with nothing to say so.
if(NOT EXECUTABLE OR NOT FILES)
	message(FATAL_ERROR "catch_tests_guard.cmake: EXECUTABLE and FILES are required")
endif()
string(REPLACE "|" ";" files "${FILES}")
foreach(file IN LISTS files)
	if(NOT EXISTS "${file}")
		message(FATAL_ERROR "${EXECUTABLE}: catch_discover_tests wrote no ${file}")
	endif()
	file(READ "${file}" listed)
	string(FIND "${listed}" "add_test(" at)
	if(at EQUAL -1)
		message(FATAL_ERROR "${EXECUTABLE}: catch_discover_tests found no test in ${file}; run ${EXECUTABLE} "
			"--list-test-names-only to see why")
	endif()
endforeach()
