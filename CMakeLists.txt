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

add_subdirectory(juce)
# vendor/json declares cmake_minimum_required(VERSION 3.1), which CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(vendor/json)
unset(CMAKE_POLICY_VERSION_MINIMUM)
#add_subdirectory(vendor/jsonrpccxx)

# Boost's own CMake package (BoostConfig.cmake, shipped since 1.70); CMake 4 has no FindBoost.
# A non-system install is found through -DBoost_ROOT=<prefix>, the BOOST_ROOT or Boost_ROOT
# environment variable, or CMAKE_PREFIX_PATH. Boost.System is header-only (Boost::headers).
find_package(Boost 1.70 CONFIG REQUIRED COMPONENTS
    program_options
	filesystem
)

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

#if(BUILD_TESTS)
#    add_subdirectory(deps/gtest)
#    add_subdirectory(test)
#endif()

#if(BUILD_COVERAGE AND NOT CMAKE_COMPILER_IS_GNUCXX)
#    message([WARNING] "Code Coverage on non-GCC compilers is not supported by default.
#Ensure that coverage library profile_rt is in CMake library path before executing.")
#endif()
