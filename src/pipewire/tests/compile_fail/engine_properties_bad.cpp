// What must not compile about the property table (docs/engine.md#properties-ear-sets): a builder takes each key from
// property<home>(), which refuses a key the table does not list for that home; a key is a PropertyKey<home>, which
// nothing but property<home>() (and seamChecked, for the card seam's home) makes; and an object's properties take only
// the keys of its own home. Compiled once with no case (it must compile: the control) and once per case, which must be
// refused for the reason src/pipewire/CMakeLists.txt names.
#include <string>

#include "engine_property_set.hpp"

namespace ear::pipewire {

PropertyKey<Home::block> listedKey() { return property<Home::block>("node.group"); }

void setsAListedKey() {
	PropertySet<Home::block> properties({{listedKey(), "x"}});
	properties.set(property<Home::block>("node.name"), "y");
}

void cardTakesItsSeam(const std::string& seamKey) {
	PropertySet<Home::card> properties({});
	properties.set(seamChecked(seamKey), "x");
}

#ifdef EAR_CASE_unlisted_key
// No row lists it anywhere.
PropertyKey<Home::block> unlisted() { return property<Home::block>("node.latency"); }
#endif

#ifdef EAR_CASE_other_home
// Listed, but for the internal clock, not for a block.
PropertyKey<Home::block> otherHome() { return property<Home::block>("resync.ms"); }
#endif

#ifdef EAR_CASE_forbidden_key
// Forbidden on a card: refused as forbidden, before any lookup.
PropertyKey<Home::card> forced() { return property<Home::card>("node.force-quantum"); }
#endif

#ifdef EAR_CASE_string_key
// A string where a key is required: no conversion makes one.
void stringKey(PropertySet<Home::block>& properties) { properties.set("node.latency", "x"); }
#endif

#ifdef EAR_CASE_pointer_key
// A pointer made into a key by hand: the constructor is property()'s alone.
void pointerKey(PropertySet<Home::block>& properties, const char* raw) {
	properties.set(PropertyKey<Home::block>(raw), "x");
}
#endif

#ifdef EAR_CASE_key_of_another_home
// A card's key set on a block's properties.
void keyOfAnotherHome(PropertySet<Home::block>& properties) { properties.set(property<Home::card>("audio.rate"), "x"); }
#endif

#ifdef EAR_CASE_seam_key_on_a_block
// The card seam's run-time key reaches only a card.
void seamKeyOnABlock(PropertySet<Home::block>& properties, const std::string& key) {
	properties.set(seamChecked(key), "x");
}
#endif

#ifdef EAR_CASE_seam_key_from_a_temporary
// The seam's key text must be storage that outlives it, never a temporary.
void seamKeyFromATemporary(PropertySet<Home::card>& properties) { properties.set(seamChecked(std::string("x")), "y"); }
#endif

#ifdef EAR_CASE_stream_arguments_elsewhere
// The RTP stream's nested arguments go only to the receiver's arguments.
void streamArgumentsElsewhere(ModuleArguments<Home::rtpStream>& stream) {
	ModuleArguments<Home::rtpStream> nested;
	stream.add(property<Home::rtpStream>("node.always-process"), nested);
}
#endif

} // namespace ear::pipewire
