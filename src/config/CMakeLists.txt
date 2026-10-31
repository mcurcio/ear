# ear_config: the graph file in knobly, mcurcio/knobly (docs/design/knobly-config.md): ear.conf loaded
# with knobly and read with ear's directives into a wanted graph, with knobly's diagnostics and ear's
# own, placed in the wire's units. knobly is private to it: its headers are internal (src/config/,
# never included by another package) and no knobly type reaches ear_control's or ear_core's.
# knobly is in compiled mode (CMakeLists.txt, KNOBLY_COMPILED), so its definitions are compiled
# once, in knobly_implementation.cpp. The ear binary does not link it yet.
add_library(ear_config STATIC
	graph_file_model.cpp
	knobly_implementation.cpp
	place.cpp)
target_link_libraries(ear_config PUBLIC ear_core ear_control PRIVATE knobly::knobly)

if(BUILD_TESTS)
	include(Catch)
	add_executable(ear_config_tests
		tests/graph_file_model_test.cpp
		tests/place_test.cpp
		tests/test_types.cpp)
	target_include_directories(ear_config_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
	target_link_libraries(ear_config_tests PRIVATE ear_config knobly::knobly ear_catch_main)
	catch_discover_tests(ear_config_tests)
endif()
