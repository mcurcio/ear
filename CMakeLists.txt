cmake_minimum_required(VERSION 3.22...4.2)

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

# Dependencies are fetched and built by this project; nothing is installed. Their sources live
# in FETCHCONTENT_BASE_DIR (default .cache/deps in the source tree, gitignored), which
# "make clean" (rm -rf out) keeps.
# Set before include(FetchContent), which otherwise caches its own default (build/_deps).
set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.cache/deps" CACHE PATH
	"Where FetchContent keeps downloaded dependency sources")
include(FetchContent)
include(cmake/KeptFetchContent.cmake)

# JUCE, pinned by commit: juce_commit is the commit the upstream 9.0.4 tag points at. A full
# commit id names exactly one tree; a tag can be moved, and GitHub does not guarantee the bytes
# (so the checksum) of its generated archives. GIT_SHALLOW clones at depth 1 with every branch
# and tag tip, which includes this commit because it is a release tag's target; a commit that
# is no tag or branch tip would fail the checkout. Bump by setting juce_commit to another
# release tag's commit (git ls-remote --tags <juce_repository>).
# .claude/skills/juce-api/scripts/ensure.sh reads juce_commit from this line.
set(juce_repository https://github.com/juce-framework/JUCE.git)
set(juce_commit 3fb5cbc0340758596dac39f7c8de47d562b4896e) # JUCE 9.0.4
kept_fetchcontent(juce "${juce_repository}\n${juce_commit}"
	GIT_REPOSITORY ${juce_repository}
	GIT_TAG ${juce_commit}
	GIT_SHALLOW TRUE
)

# vendor/json declares cmake_minimum_required(VERSION 3.1), which CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(vendor/json)
unset(CMAKE_POLICY_VERSION_MINIMUM)
#add_subdirectory(vendor/jsonrpccxx)

# Boost is built by this project from the official Boost CMake release, like JUCE:
# nothing is installed. Its download and extracted source live in FETCHCONTENT_BASE_DIR, so
# "make clean" does not re-download; the next configure re-extracts the kept archive. Boost's
# build tree stays in the build directory.
set(BOOST_INCLUDE_LIBRARIES program_options uuid endian asio beast)
FetchContent_Declare(boost
	URL https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz
	URL_HASH SHA256=9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/boost-build"
)
FetchContent_MakeAvailable(boost)

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
