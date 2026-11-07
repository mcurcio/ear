# ear_all_node_types: the one list of node types, generated at configure from the folders src/nodes/CMakeLists.txt found
# (prior art: LLVM's generated target lists). A folder's name gives its header, its factory and its library. main.cpp and
# the all-type tests call this and link it alone. It lives outside src/nodes, which holds node types and nothing else.
get_property(types GLOBAL PROPERTY EAR_NODE_TYPES)

set(EAR_NODE_TYPE_INCLUDES "")
set(EAR_NODE_TYPE_PUSHES "")
set(libraries "")
foreach(type IN LISTS types)
	string(SUBSTRING "${type}" 0 1 first)
	string(SUBSTRING "${type}" 1 -1 rest)
	string(TOUPPER "${first}" first)
	string(APPEND EAR_NODE_TYPE_INCLUDES "#include \"ear/nodes/${type}.hpp\"\n")
	string(APPEND EAR_NODE_TYPE_PUSHES "\ttypes.push_back(make${first}${rest}NodeType(folders));\n")
	list(APPEND libraries ear_node_${type})
endforeach()

# Rewritten only when its text changes, so a configure rebuilds nothing unless the list did.
configure_file(all_node_types.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/all_node_types.cpp" @ONLY)

add_library(ear_all_node_types STATIC "${CMAKE_CURRENT_BINARY_DIR}/all_node_types.cpp")
# Header set and its standalone compile check: see src/core/CMakeLists.txt.
target_sources(ear_all_node_types PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/all_node_types/all_node_types.hpp)
# EAR_NODE_TYPES: the folders, for test/nodes, which is added before this directory.
set_target_properties(ear_all_node_types PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON EAR_NODE_TYPES "${types}")
target_link_libraries(ear_all_node_types PUBLIC ear_core PRIVATE ${libraries})
