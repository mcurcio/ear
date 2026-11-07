# The node types, each its own library in its own folder (docs/design/batch2.md section 4, docs/design/rtp.md,
# docs/design/sendspin.md). A folder here is a type, found at configure and looked for again on every build; its
# CMakeLists.txt calls ear_node_type(), and src/all_node_types lists what was found. No type links another.
include("${PROJECT_SOURCE_DIR}/cmake/NodeType.cmake")

file(GLOB entries CONFIGURE_DEPENDS LIST_DIRECTORIES true RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(entry IN LISTS entries)
	# This file, and any dotfile (Finder's .DS_Store through the Mac's mount).
	if(entry STREQUAL "CMakeLists.txt" OR entry MATCHES "^\\.")
		continue()
	endif()
	if(NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${entry}")
		message(FATAL_ERROR "src/nodes/ holds node types and nothing else: ${entry}")
	endif()
	add_subdirectory(${entry})
	get_property(types GLOBAL PROPERTY EAR_NODE_TYPES)
	if(NOT entry IN_LIST types)
		message(FATAL_ERROR "src/nodes/${entry}: its CMakeLists.txt must call ear_node_type()")
	endif()
endforeach()
ear_refuse_node_type_links(${types})
