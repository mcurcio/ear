cmake_minimum_required(VERSION 3.28...4.2)

# The version is release-please's (release-please-config.json): its release pull request
# rewrites the number on the marked line.
project(EAR VERSION 0.1.0) # x-release-please-version

# EAR builds on Linux only: it links PipeWire's libraries. On a Mac (or a Linux host without
# the packages), build in the image: scripts/dev.sh make test. Any other platform stops here,
# before anything is fetched.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	message(FATAL_ERROR "EAR builds on Linux only (this is ${CMAKE_SYSTEM_NAME}); "
		"build in the Linux image instead: scripts/dev.sh make test")
endif()

option(BUILD_TESTS "Build project with test suite" OFF)

#set(CMAKE_BUILD_TYPE Debug)
#set(CMAKE_BUILD_TYPE Release)

#include_directories(include deps/msgpack/include)

#set(msgpack_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/msgpack/src")

add_definitions("-g -W -Wall -O2 -fPIC")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

# Dependencies are fetched and built by this project; nothing is installed. Their sources live
# in FETCHCONTENT_BASE_DIR (default .cache/deps in the source tree, gitignored), which
# "make clean" (rm -rf out) keeps.
# Set before include(FetchContent), which otherwise caches its own default (build/_deps).
set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.cache/deps" CACHE PATH
	"Where FetchContent keeps downloaded dependency sources")
include(FetchContent)
include(cmake/KeptFetchContent.cmake)

# vendor/json declares cmake_minimum_required(VERSION 3.1), which CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
# SYSTEM: its headers' warnings (3.7 under C++23: std::is_pod deprecated) are not EAR's.
add_subdirectory(vendor/json SYSTEM)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# Boost is built by this project from the official Boost CMake release: nothing is installed.
# kept_fetchcontent keeps the download and the extracted 800 MB tree across "make clean"
# (re-extracting it costs 30-40 s); Boost's build tree stays in the build directory.
# EXCLUDE_FROM_ALL builds only the Boost targets EAR links. BOOST_INCLUDE_LIBRARIES names the
# libraries EAR uses (Boost adds their dependencies): program_options parses the command line;
# beast (header-only, with Asio) serves the control port.
set(BOOST_INCLUDE_LIBRARIES program_options beast)
set(boost_url
	https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz)
set(boost_sha256 9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d)
kept_fetchcontent(boost "${boost_url}\n${boost_sha256}"
	URL ${boost_url}
	URL_HASH SHA256=${boost_sha256}
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	EXCLUDE_FROM_ALL
)
# The Boost libraries EAR's targets link must come out of Boost's configure as targets. This
# checks them by name, not by the list above, so one dropped from that list (or missing from a
# Boost release) stops configure here, naming it, instead of failing at a target that links it.
foreach(boost_library IN ITEMS "program_options Boost.ProgramOptions" "beast Boost.Beast")
	separate_arguments(boost_library)
	list(GET boost_library 0 target)
	list(GET boost_library 1 library_name)
	if(NOT TARGET Boost::${target})
		message(FATAL_ERROR "${library_name} (Boost::${target}) is not built: add ${target} to "
			"BOOST_INCLUDE_LIBRARIES (or bump Boost to a release that has it)")
	endif()
endforeach()
# Boost::program_options links Boost::lexical_cast, which links the compiled Boost::container
# but uses only its header-only container_fwd.hpp. Take container for compilation only there,
# so program_options stays the only compiled Boost library.
get_target_property(boost_lexical_cast_target Boost::lexical_cast ALIASED_TARGET)
get_target_property(boost_lexical_cast_links ${boost_lexical_cast_target} INTERFACE_LINK_LIBRARIES)
list(TRANSFORM boost_lexical_cast_links REPLACE "^Boost::container$"
	"$<COMPILE_ONLY:Boost::container>")
set_target_properties(${boost_lexical_cast_target} PROPERTIES
	INTERFACE_LINK_LIBRARIES "${boost_lexical_cast_links}")

# System libraries, found with pkg-config (the image, docker/Dockerfile, installs them):
# PipeWire for the engine and libsndfile for the file node. Each is an imported target,
# PkgConfig::<PREFIX>, global so the packages under src/ link it. Configure stops naming every
# package it did not find. FindPkgConfig caches a found result and skips the lookup on later
# configures, so a package removed after a successful configure is reported only once
# out/CMakeCache.txt is deleted (make clean); a failed lookup is retried on every configure.
find_package(PkgConfig REQUIRED)
set(missing_packages "")
foreach(package IN ITEMS "PIPEWIRE libpipewire-0.3" "SNDFILE sndfile")
	separate_arguments(package)
	list(GET package 0 prefix)
	list(GET package 1 module)
	pkg_check_modules(${prefix} QUIET IMPORTED_TARGET GLOBAL ${module})
	if(NOT ${prefix}_FOUND)
		list(APPEND missing_packages ${module})
	endif()
endforeach()
if(missing_packages)
	list(JOIN missing_packages ", " missing_packages)
	message(FATAL_ERROR "pkg-config found no ${missing_packages}. Build in the image "
		"(scripts/dev.sh make test), whose docker/Dockerfile installs them.")
endif()

#add_subdirectory(benchmark)
#add_subdirectory(build)
#add_subdirectory(bin)

# Tests are on before src/ so its packages' test directories register theirs with CTest;
# test/ holds ear_catch_main, the Catch2 runner every test executable links, and the tests of
# the ear binary itself. Catch.cmake (catch_discover_tests) is on the module path for them.
if(BUILD_TESTS)
	enable_testing()
	list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vendor/catch/contrib")
	add_subdirectory(test)
endif()

add_subdirectory(src)

#if(BUILD_COVERAGE AND NOT CMAKE_COMPILER_IS_GNUCXX)
#    message([WARNING] "Code Coverage on non-GCC compilers is not supported by default.
#Ensure that coverage library profile_rt is in CMake library path before executing.")
#endif()
