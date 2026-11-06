# carried_patch_missing(<out> <tree> <patch>): sets <out> empty if <tree> carries every hunk of <patch>, else to why not.
# Asking git whether the patch would reverse holds for any version of a patch: a tree with an older one, or only part of
# it, does not pass, as one with a marker it kept from that version would. git looks no higher than <tree>, as in a
# subfolder of another repository `git apply` skips paths it counts as outside that subfolder, and says it succeeded.
function(carried_patch_missing out tree patch)
	get_filename_component(above "${tree}" DIRECTORY)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${above}"
			git apply --reverse --check "${patch}"
		WORKING_DIRECTORY "${tree}"
		RESULT_VARIABLE result
		ERROR_VARIABLE detail
		OUTPUT_QUIET
	)
	if(result EQUAL 0)
		set(${out} "" PARENT_SCOPE)
	else()
		set(${out} "git apply --reverse says ${result}: ${detail}" PARENT_SCOPE)
	endif()
endfunction()
