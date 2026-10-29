# kept_fetchcontent(<name> <pin> <FetchContent_Declare options>...)
#
# FetchContent_Declare and FetchContent_MakeAvailable for a dependency whose source tree is
# kept across "make clean": FETCHCONTENT_BASE_DIR/<name>-src (default .cache/deps, gitignored)
# outlives the build directory, and the build tree goes to <build dir>/_deps/<name>-build.
#
# FetchContent keeps its population stamps in the build directory, so after "make clean" it
# would download or extract the tree again. A marker written into the tree after a verified
# population records <pin> (whatever fixes the content: an archive URL and SHA-256, or a
# repository and commit). When the marker matches the declared pin, the kept tree is used as
# FETCHCONTENT_SOURCE_DIR_<NAME> (FetchContent's documented override) and nothing is fetched.
# A missing or different marker (no tree, interrupted population, another pin) falls back to
# the verified fetch, which replaces the tree and its marker. An explicit
# -DFETCHCONTENT_SOURCE_DIR_<NAME>=... still wins. The empty cache entry is created here so
# the kept path stays a normal variable, re-checked on every configure.
function(kept_fetchcontent name pin)
	string(TOLOWER "${name}" lc)
	string(TOUPPER "${name}" uc)
	set(FETCHCONTENT_SOURCE_DIR_${uc} "" CACHE PATH
		"When not empty, overrides where to find pre-populated content for ${lc}")
	set(user_override "${FETCHCONTENT_SOURCE_DIR_${uc}}")
	set(kept_dir "${FETCHCONTENT_BASE_DIR}/${lc}-src")
	set(marker "${kept_dir}/.ear-populated-from")
	set(marker_content "${pin}\n")
	if(NOT user_override AND EXISTS "${marker}")
		file(READ "${marker}" marker_found)
		if(marker_found STREQUAL marker_content)
			set(FETCHCONTENT_SOURCE_DIR_${uc} "${kept_dir}")
		endif()
	endif()
	if(user_override)
		message(STATUS "${lc}: using FETCHCONTENT_SOURCE_DIR_${uc}=${user_override}")
	elseif(FETCHCONTENT_SOURCE_DIR_${uc})
		message(STATUS "${lc}: reusing ${kept_dir} (marker matches the pin)")
	else()
		message(STATUS "${lc}: fetching into ${kept_dir}")
	endif()
	FetchContent_Declare(${lc}
		${ARGN}
		BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/${lc}-build"
	)
	FetchContent_MakeAvailable(${lc})
	if(NOT FETCHCONTENT_SOURCE_DIR_${uc} AND ${lc}_SOURCE_DIR STREQUAL kept_dir)
		file(WRITE "${marker}" "${marker_content}")
	endif()
	set(${lc}_SOURCE_DIR "${${lc}_SOURCE_DIR}" PARENT_SCOPE)
	set(${lc}_BINARY_DIR "${${lc}_BINARY_DIR}" PARENT_SCOPE)
endfunction()
