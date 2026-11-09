# ear_node_rtp: the "rtp" node type (docs/node-types.md#rtp). It reaches the engine only through ear_core's interfaces:
# an RTP receiver is asked for through the node's grant.
ear_node_type(SOURCES rtp.cpp TESTS tests/rtp_test.cpp)
