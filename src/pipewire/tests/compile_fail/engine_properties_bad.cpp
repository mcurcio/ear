// What must not compile about the property table (docs/engine.md#properties-ear-sets): a builder takes each key from
// property(), which refuses a key the table does not list for that home, and a key is a PropertyKey, which nothing but
// property() makes from a string. Compiled once with no case (it must compile: the control) and once per case, which
// must be refused for the reason src/pipewire/CMakeLists.txt names.
#include "engine_property_set.hpp"

namespace ear::pipewire {

PropertyKey listedKey() { return property(Home::block, "node.group"); }

void setsAListedKey(pw_properties* properties) { setProperty(properties, listedKey(), "x"); }

#ifdef EAR_CASE_unlisted_key
// No row lists it anywhere.
PropertyKey unlisted() { return property(Home::block, "node.latency"); }
#endif

#ifdef EAR_CASE_other_home
// Listed, but for the internal clock, not for a block.
PropertyKey otherHome() { return property(Home::block, "resync.ms"); }
#endif

#ifdef EAR_CASE_forbidden_key
// Forbidden on a card: refused as forbidden, before any lookup.
PropertyKey forced() { return property(Home::card, "node.force-quantum"); }
#endif

#ifdef EAR_CASE_string_key
// A string where a key is required: no conversion makes one.
void stringKey(pw_properties* properties) { setProperty(properties, "node.latency", "x"); }
#endif

#ifdef EAR_CASE_pointer_key
// A pointer made into a key by hand: the constructor is property()'s alone.
void pointerKey(pw_properties* properties, const char* raw) { setProperty(properties, PropertyKey(raw), "x"); }
#endif

} // namespace ear::pipewire
