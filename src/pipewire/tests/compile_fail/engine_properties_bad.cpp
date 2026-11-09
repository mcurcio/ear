// What must not compile about the property table (docs/engine.md#properties-ear-sets): a builder takes each key from
// property(), which refuses a key the table does not list for that home. Compiled once with no case (it must compile:
// the control) and once per case, which must be refused for the reason src/pipewire/CMakeLists.txt names.
#include "engine_properties.hpp"

namespace ear::pipewire {

const char* listedKey() { return property(Home::block, "node.group"); }

#ifdef EAR_CASE_unlisted_key
// No row lists it anywhere.
const char* unlisted() { return property(Home::block, "node.latency"); }
#endif

#ifdef EAR_CASE_other_home
// Listed, but for the internal clock, not for a block.
const char* otherHome() { return property(Home::block, "resync.ms"); }
#endif

#ifdef EAR_CASE_forbidden_key
// Forbidden on a card: no row may list it there, so property() finds none.
const char* forced() { return property(Home::card, "node.force-quantum"); }
#endif

} // namespace ear::pipewire
