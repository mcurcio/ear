# Run by the test ear_node_types_none_refused (test/nodes/CMakeLists.txt): src/all_node_types read before any folder filled
# its list, which must stop configure rather than build an ear that knows no types.
cmake_minimum_required(VERSION 3.28...4.2)
project(NoNodeTypes LANGUAGES NONE)
add_subdirectory("${EAR_ALL_NODE_TYPES_DIR}" all_node_types)
