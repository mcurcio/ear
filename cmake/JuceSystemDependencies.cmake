# ear_require_juce_system_dependencies(<juce source dir> <consumer> <module or target>...)
#
# Stops configure, naming each missing item, the JUCE module that needs it and the install
# script (on macOS, the SDK searched), when a system dependency of the given JUCE modules is
# missing on this platform.
# <consumer> only labels the message (juceaide, ear). Names that are not JUCE modules (no module
# header, such as juce_recommended_config_flags) are skipped, so a link list can be passed whole;
# configure stops when none of the names is a module, so a JUCE layout change cannot make the
# check pass while checking nothing. Only macOS and Linux are handled: the platform gate at the
# top of CMakeLists.txt stops every other platform before this runs.
#
# JUCE declares each module's system dependencies in its header and passes them on itself, but
# without failing when one is missing (JUCEModuleSupport.cmake: pkg_check_modules without
# REQUIRED, plain -framework flags), so a missing package only surfaces later: on Linux as a
# compiler error naming a header, on macOS as a linker error ("framework not found").
# This walks each module's "dependencies:" transitively and requires, for this platform only:
# - Linux: each linuxPackages entry, found with pkg-config, as JUCE looks it up.
# - macOS: each OSXFrameworks and WeakOSXFrameworks entry, in the active SDK. JUCE links the weak
#   ones with -weak_framework, which makes them optional at run time but still needs them in the
#   SDK at link time.
# linuxLibs (juce_core: rt dl pthread) and OSXLibs are libraries the C toolchain ships, not
# checked. Nor are the libraries JUCE adds per target for juce_add_* options (NEEDS_CURL,
# NEEDS_WEB_BROWSER, NEEDS_OPENGL_ES, NEEDS_STORE_KIT in JUCEUtils.cmake): EAR sets none of them;
# one that is turned on needs its packages added here. Every configure looks each item up again
# (no cached hit is reused), so a package removed since the last configure is reported.
# The JUCE CMake helpers that read these headers (_juce_extract_metadata_block) are private, so
# the parser below follows JUCE's module format (docs/JUCE Module Format.md and that function):
# "key: values" lines between BEGIN_ and END_JUCE_MODULE_DECLARATION, values separated by spaces
# or commas, a line without a key continuing the previous key. A JUCE bump must re-check it
# against JUCEModuleSupport.cmake.
set(ear_build_deps_script "${CMAKE_CURRENT_LIST_DIR}/../scripts/install-build-deps.sh")
cmake_path(NORMAL_PATH ear_build_deps_script)

# Sets <out_prefix>_<key> in the caller for each key of <header>'s module declaration.
function(_ear_juce_module_declaration header out_prefix)
	file(STRINGS "${header}" lines)
	set(in_block NO)
	set(key "")
	set(keys "")
	foreach(line IN LISTS lines)
		if(NOT in_block)
			if(line MATCHES "BEGIN_JUCE_MODULE_DECLARATION")
				set(in_block YES)
			endif()
			continue()
		endif()
		if(line MATCHES "END_JUCE_MODULE_DECLARATION")
			break()
		endif()
		if(line MATCHES "^[\t ]*([a-zA-Z]+):(.*)$")
			set(key "${CMAKE_MATCH_1}")
			set(line "${CMAKE_MATCH_2}")
			list(APPEND keys "${key}")
		endif()
		if(NOT key STREQUAL "")
			string(REGEX REPLACE "[\t ,]+" ";" values "${line}")
			list(REMOVE_ITEM values "")
			list(APPEND value_${key} ${values})
		endif()
	endforeach()
	foreach(key IN LISTS keys)
		set(${out_prefix}_${key} "${value_${key}}" PARENT_SCOPE)
	endforeach()
endfunction()

function(ear_require_juce_system_dependencies juce_dir consumer)
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		set(keys linuxPackages)
	elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
		set(keys OSXFrameworks WeakOSXFrameworks)
	else()
		message(FATAL_ERROR "ear_require_juce_system_dependencies has no keys for ${CMAKE_SYSTEM_NAME}")
	endif()

	# Transitive closure over "dependencies:", each module read once.
	set(pending ${ARGN})
	set(modules "")
	set(needs "")
	while(pending)
		list(POP_FRONT pending module)
		if(module IN_LIST modules)
			continue()
		endif()
		set(header "")
		foreach(dir IN ITEMS "${juce_dir}/modules/${module}" "${juce_dir}/extras/Build/${module}")
			foreach(candidate IN ITEMS "${dir}/${module}.h" "${dir}/${module}.hpp")
				if(NOT header AND EXISTS "${candidate}")
					set(header "${candidate}")
				endif()
			endforeach()
		endforeach()
		if(NOT header)
			continue()
		endif()
		list(APPEND modules "${module}")
		_ear_juce_module_declaration("${header}" decl)
		list(APPEND pending ${decl_dependencies})
		foreach(key IN LISTS keys)
			foreach(item IN LISTS decl_${key})
				list(APPEND needs "${item}|${module}")
			endforeach()
			unset(decl_${key})
		endforeach()
		unset(decl_dependencies)
	endwhile()
	if(NOT modules)
		list(JOIN ARGN ", " names)
		message(FATAL_ERROR "${consumer}: none of ${names} has a module header under ${juce_dir}; "
			"re-check this parser against the JUCE release juce_commit names")
	endif()
	list(JOIN modules ", " modules)
	message(STATUS "${consumer}: JUCE modules ${modules}")

	set(missing "")
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND needs)
		find_package(PkgConfig QUIET)
		if(NOT PKG_CONFIG_FOUND)
			list(APPEND missing "pkg-config (to find ${consumer}'s JUCE packages)")
		endif()
	endif()
	set(checked "")
	foreach(need IN LISTS needs)
		string(REPLACE "|" ";" need "${need}")
		list(GET need 0 item)
		list(GET need 1 module)
		if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
			if(NOT PKG_CONFIG_FOUND)
				list(APPEND missing "pkg-config package ${item}, for ${module}")
				continue()
			endif()
			# pkg_check_modules skips the lookup while its cached <prefix>_FOUND is true.
			string(MAKE_C_IDENTIFIER "ear_sysdep_${item}" var)
			unset(${var}_FOUND CACHE)
			pkg_check_modules(${var} QUIET "${item}")
			set(found "${${var}_FOUND}")
			set(what "pkg-config package")
		else()
			set(CMAKE_FIND_FRAMEWORK ONLY)
			unset(found) # find_library skips the search when its variable is already set.
			find_library(found NAMES "${item}" NO_CACHE)
			set(what "framework")
		endif()
		if(found)
			list(APPEND checked "${item}")
		else()
			list(APPEND missing "${what} ${item}, for ${module}")
		endif()
	endforeach()

	if(missing)
		list(JOIN missing "\n  " missing)
		if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
			# The frameworks ship in the macOS SDK, so a missing one means a wrong or incomplete SDK.
			if(CMAKE_OSX_SYSROOT)
				set(sdk "${CMAKE_OSX_SYSROOT}")
			else()
				set(sdk "the default one (xcrun --show-sdk-path)")
			endif()
			set(advice "The SDK searched is ${sdk}. Install the Xcode Command Line Tools "
				"(xcode-select --install) or select a complete Xcode (xcode-select -s <path>), "
				"then configure again.")
		else()
			set(advice "Install them with ${ear_build_deps_script}, then configure again.")
		endif()
		message(FATAL_ERROR "${consumer} needs system dependencies that are missing:\n"
			"  ${missing}\n" ${advice})
	endif()
	list(REMOVE_DUPLICATES checked)
	list(JOIN checked ", " checked)
	if(NOT checked)
		set(checked "(none needed)")
	endif()
	message(STATUS "${consumer}: found system dependencies ${checked}")
endfunction()
