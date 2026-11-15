# ear_mdns: a node's mDNS listing (docs/architecture.md#packages): one service instance announced, answered and said
# goodbye to over its own socket, run on whichever loop of a node type keeps it. Its one outside library, mjansson/mdns
# (one header, cmake/MdnsDeps.cmake), is private to advertiser.cpp. It depends on nothing of ear's; node types link it
# (it is no package only main.cpp links, so cmake/NodeType.cmake allows it).
add_library(ear_mdns STATIC advertiser.cpp)
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_mdns PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/mdns/advertiser.hpp)
set_target_properties(ear_mdns PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_include_directories(ear_mdns SYSTEM PRIVATE "${mdns_SOURCE_DIR}")

if(BUILD_TESTS)
	# Real sockets on the address the kernel routes the mDNS group through; the tests read the answers with mdns.h too,
	# so its include path is theirs as well as advertiser.cpp's.
	add_executable(ear_mdns_tests tests/advertiser_test.cpp)
	target_link_libraries(ear_mdns_tests PRIVATE ear_mdns ear_catch_main)
	target_include_directories(ear_mdns_tests SYSTEM PRIVATE "${mdns_SOURCE_DIR}")
	add_dependencies(ear_mdns_tests ear_mdns_verify_interface_header_sets)
	ear_catch_tests(ear_mdns_tests)
endif()
