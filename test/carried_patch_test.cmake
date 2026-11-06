# Run with cmake -DWORK_DIR=<folder> -P: what configure asks of a tree named for a dependency with a carried patch
# (cmake/CarriedPatch.cmake). The trees sit in a folder of another repository, as one in ear's own checkout does, where
# `git apply` left to itself skips the patch and exits 0.
cmake_policy(VERSION 3.28)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CarriedPatch.cmake")

# A new folder each run: Docker's Mac file share can lose a folder removed and made again under one name.
string(RANDOM LENGTH 16 run)
set(WORK_DIR "${WORK_DIR}/${run}")
file(MAKE_DIRECTORY "${WORK_DIR}")
execute_process(COMMAND git init --quiet WORKING_DIRECTORY "${WORK_DIR}" COMMAND_ERROR_IS_FATAL ANY)

function(write_patch path)
	set(body "")
	foreach(name IN LISTS ARGN)
		string(APPEND body
			"diff --git a/${name} b/${name}\n--- a/${name}\n+++ b/${name}\n@@ -1 +1 @@\n-before\n+after\n")
	endforeach()
	file(WRITE "${path}" "${body}")
endfunction()

# A tree whose file `one` and file `two` read `after` if named here, else `before`.
function(write_tree name)
	foreach(file IN ITEMS one two)
		if("${file}" IN_LIST ARGN)
			file(WRITE "${WORK_DIR}/${name}/${file}" "after\n")
		else()
			file(WRITE "${WORK_DIR}/${name}/${file}" "before\n")
		endif()
	endforeach()
endfunction()

function(expect tree wanted)
	carried_patch_missing(missing "${WORK_DIR}/${tree}" "${WORK_DIR}/patch")
	if(wanted STREQUAL "carries" AND missing)
		message(FATAL_ERROR "${tree} carries the patch, and was told: ${missing}")
	elseif(wanted STREQUAL "lacks" AND NOT missing)
		message(FATAL_ERROR "${tree} lacks the patch, and passed")
	endif()
endfunction()

write_patch("${WORK_DIR}/patch" one two)
write_tree(whole one two)
write_tree(pristine)
write_tree(older one)   # an older version of the patch, which touched one file only

expect(whole carries)
expect(pristine lacks)
expect(older lacks)
expect(no-such-tree lacks)

# With a repository of its own, as a fetched tree has.
execute_process(COMMAND git init --quiet WORKING_DIRECTORY "${WORK_DIR}/whole" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND git init --quiet WORKING_DIRECTORY "${WORK_DIR}/pristine" COMMAND_ERROR_IS_FATAL ANY)
expect(whole carries)
expect(pristine lacks)

file(REMOVE_RECURSE "${WORK_DIR}")
