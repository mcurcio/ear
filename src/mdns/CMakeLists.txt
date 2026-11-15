# ear_mdns: a node's mDNS listing (docs/architecture.md#packages): one service instance announced, answered and said
# goodbye to over its own socket, run on whichever loop of a node type keeps it. Its one outside library, mjansson/mdns
# (one header), is private to advertiser.cpp. It depends on nothing of ear's; node types link it (it is no package only
# main.cpp links, so cmake/NodeType.cmake allows it).
#
# Interfaces only until its slice lands advertiser.cpp and its tests: an INTERFACE library whose header set is compiled
# on its own (VERIFY_INTERFACE_HEADER_SETS), linked by nothing yet.
add_library(ear_mdns INTERFACE)
target_sources(ear_mdns INTERFACE FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/mdns/advertiser.hpp)
set_target_properties(ear_mdns PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
