cmake_minimum_required(VERSION 3.28...4.2)

project(EAR VERSION 0.0.1)

option(BUILD_TESTS "Build project with test suite" OFF)

#set(CMAKE_BUILD_TYPE Debug)
#set(CMAKE_BUILD_TYPE Release)

#include_directories(include deps/msgpack/include)

#set(msgpack_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/msgpack/src")

add_definitions("-g -W -Wall -O2 -fPIC")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

add_subdirectory(juce)
# vendor/json declares cmake_minimum_required(VERSION 3.1), which CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(vendor/json)
unset(CMAKE_POLICY_VERSION_MINIMUM)
#add_subdirectory(vendor/jsonrpccxx)

# Boost is built by this project from the official Boost CMake release, like JUCE:
# nothing is installed. The download and extracted source live in FETCHCONTENT_BASE_DIR
# (default .cache/deps in the source tree, gitignored), so "make clean" (which removes out/)
# keeps them. Boost's build tree stays in the build directory.
# Set before include(FetchContent), which otherwise caches its own default (build/_deps).
set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.cache/deps" CACHE PATH
	"Where FetchContent keeps downloaded dependency sources")
include(FetchContent)
set(BOOST_INCLUDE_LIBRARIES program_options uuid endian asio beast)
set(boost_url https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz)
set(boost_sha256 9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d)
FetchContent_Declare(boost
	URL ${boost_url}
	URL_HASH SHA256=${boost_sha256}
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/boost-build"
	EXCLUDE_FROM_ALL
)
# FetchContent keeps its population stamps in the build directory, so after "make clean" it
# would re-extract the 800 MB tree (30-40 s). A marker written into the tree after a verified
# population records the archive's URL and SHA-256; when it matches the pinned pair, the kept
# tree is used as FETCHCONTENT_SOURCE_DIR_BOOST (FetchContent's documented override) and nothing
# is extracted. A missing or different marker (no tree, interrupted extraction, other pin)
# falls back to the verified download and extraction, which replaces the tree and its marker.
# An explicit -DFETCHCONTENT_SOURCE_DIR_BOOST=... still wins. The empty cache entry is created
# here so the kept path below stays a normal variable, re-checked on every configure.
set(FETCHCONTENT_SOURCE_DIR_BOOST "" CACHE PATH
	"When not empty, overrides where to find pre-populated content for boost")
set(boost_kept_dir "${FETCHCONTENT_BASE_DIR}/boost-src")
set(boost_marker "${boost_kept_dir}/.ear-populated-from")
set(boost_marker_content "${boost_url}\n${boost_sha256}\n")
if(NOT FETCHCONTENT_SOURCE_DIR_BOOST AND EXISTS "${boost_marker}")
	file(READ "${boost_marker}" boost_marker_found)
	if(boost_marker_found STREQUAL boost_marker_content)
		set(FETCHCONTENT_SOURCE_DIR_BOOST "${boost_kept_dir}")
	endif()
endif()
FetchContent_MakeAvailable(boost)
if(NOT FETCHCONTENT_SOURCE_DIR_BOOST AND boost_SOURCE_DIR STREQUAL boost_kept_dir)
	file(WRITE "${boost_marker}" "${boost_marker_content}")
endif()
# Boost::program_options links Boost::lexical_cast, which links the compiled Boost::container
# but uses only its header-only container_fwd.hpp. Take container for compilation only there,
# so program_options stays the only compiled Boost library (src/CMakeLists.txt lists the rest).
get_target_property(boost_lexical_cast_target Boost::lexical_cast ALIASED_TARGET)
get_target_property(boost_lexical_cast_links ${boost_lexical_cast_target} INTERFACE_LINK_LIBRARIES)
list(TRANSFORM boost_lexical_cast_links REPLACE "^Boost::container$" "$<COMPILE_ONLY:Boost::container>")
set_target_properties(${boost_lexical_cast_target} PROPERTIES
	INTERFACE_LINK_LIBRARIES "${boost_lexical_cast_links}")

#find_package(JUCE REQUIRED PATHS "${CMAKE_CURRENT_LIST_DIR}/out/juce/")

message("JUCE=${JUCE_FOUND}")

include_directories(
	"${CMAKE_CURRENT_SOURCE_DIR}/vendor/json/include"
	"${CMAKE_CURRENT_SOURCE_DIR}/vendor/jsonrpccxx/include"
)
#include_directories(${Boost_INCLUDE_DIRS} ${RTTL_INCLUDE_DIRS})

#add_definitions(-DMLOG_DIRECTORY_PATH="${CMAKE_CURRENT_LIST_DIR}")

#set(CMAKE_MACOSX_RPATH False)

include_directories(include)

#add_subdirectory(benchmark)
#add_subdirectory(build)
#add_subdirectory(bin)
add_subdirectory(src)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

#if(BUILD_COVERAGE AND NOT CMAKE_COMPILER_IS_GNUCXX)
#    message([WARNING] "Code Coverage on non-GCC compilers is not supported by default.
#Ensure that coverage library profile_rt is in CMake library path before executing.")
#endif()
