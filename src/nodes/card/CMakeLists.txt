# ear_node_card: the "card" node type (docs/design/batch2.md section 4.4). It reaches the engine only through ear_core's
# interfaces: a sound card is asked for through the node's grant.
ear_node_type(SOURCES card.cpp TESTS tests/card_test.cpp)
