# ear_node_gain: the "gain" node type, a gain per channel with every adjustment live but the channel count
# (docs/node-types.md#gain).
ear_node_type(SOURCES gain.cpp TESTS tests/gain_test.cpp)
