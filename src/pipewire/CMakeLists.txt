# ear_pipewire: PipeWire's engine inside EAR, and the adapters that implement ear_core's
# interfaces with it: port blocks, links, live ports and handoffs
# (docs/design/m1-white-to-file.md section 1).
add_library(ear_pipewire STATIC
	engine.cpp
	handoffs.cpp
	links.cpp
	live_graph.cpp
	port_blocks.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt. The headers
# declare PipeWire's types they name, so PipeWire's own headers stay private to this package.
target_sources(ear_pipewire PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/pipewire/engine.hpp
	include/ear/pipewire/handoffs.hpp
	include/ear/pipewire/links.hpp
	include/ear/pipewire/live_graph.hpp
	include/ear/pipewire/port_blocks.hpp)
set_target_properties(ear_pipewire PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_pipewire PUBLIC ear_core PRIVATE PkgConfig::PIPEWIRE)

# The engine's configuration. The ear binary reads the copy beside out/ear (main passes
# <its directory>/engine.conf; Engine does no lookup of its own). configure_file copies it
# again whenever the source changes.
configure_file(engine.conf "${PROJECT_BINARY_DIR}/engine.conf" COPYONLY)

if(BUILD_TESTS)
	include(Catch)
	add_executable(ear_pipewire_tests
		tests/engine_test.cpp
		tests/handoff_test.cpp)
	# PipeWire directly: the tests drive the engine's loops and sync with its core themselves.
	target_link_libraries(ear_pipewire_tests PRIVATE ear_pipewire ear_catch_main
		PkgConfig::PIPEWIRE)
	# The tests run the engine on the source tree's engine.conf.
	target_compile_definitions(ear_pipewire_tests PRIVATE
		EAR_ENGINE_CONF="${CMAKE_CURRENT_SOURCE_DIR}/engine.conf")
	add_dependencies(ear_pipewire_tests ear_pipewire_verify_interface_header_sets)
	catch_discover_tests(ear_pipewire_tests)

	add_test(NAME ear_engine_conf_beside_ear COMMAND ${CMAKE_COMMAND} -E compare_files
		"${CMAKE_CURRENT_SOURCE_DIR}/engine.conf" "${PROJECT_BINARY_DIR}/engine.conf")
endif()
