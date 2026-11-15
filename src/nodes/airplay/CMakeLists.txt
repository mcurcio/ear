# ear_node_airplay: the "airplay" node type (docs/node-types.md#airplay): an AirPlay 1 or AirPlay 2 speaker
# (its airplay2 setting), one shairport-sync per node run as the configuration's program, its audio joined through a
# pipe receiver and its listing announced by ear_mdns. It reaches the engine and the process only through ear_core's
# interfaces.
ear_node_type(SOURCES airplay.cpp reports.cpp TESTS tests/airplay_test.cpp tests/reports_test.cpp)
target_link_libraries(ear_node_airplay PRIVATE ear_mdns) # its listing (docs/node-types.md#mdns)

if(BUILD_TESTS)
	# Its private headers name ear_mdns's Listing and Advertiser: the report loop's tests open a real one.
	target_link_libraries(ear_node_airplay_tests PRIVATE ear_mdns)
endif()
