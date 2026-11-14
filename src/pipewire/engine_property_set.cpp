// The module half of engine_property_set.hpp: a value checked to be one value, and the one module load with arguments
// (docs/engine.md#properties-ear-sets).
#include "engine_property_set.hpp"

#include <cctype>

#include <pipewire/impl.h>

namespace ear::pipewire {

void requireOneValue(const std::string& value) {
	if (value.empty()) throw std::logic_error("a module argument's value is empty");
	int depth = 0;
	bool quoted = false;
	bool escaped = false;
	for (const char c : value) {
		if (quoted) {
			if (escaped) escaped = false;
			else if (c == '\\') escaped = true;
			else if (c == '"') quoted = false;
			continue;
		}
		if (c == '"') quoted = true;
		else if (c == '{' || c == '[') ++depth;
		else if (c == '}' || c == ']') --depth;
		else if (depth == 0 && std::isspace(static_cast<unsigned char>(c)) != 0)
			throw std::logic_error("a module argument's value would open another key: " + value);
		if (depth < 0) throw std::logic_error("a module argument's value closes what it did not open: " + value);
	}
	if (quoted || depth != 0) throw std::logic_error("a module argument's value is left open: " + value);
}

pw_impl_module* loadModule(pw_context* context, const char* name, const ModuleArguments<Home::rtpReceiver>& arguments) {
	return pw_context_load_module(context, name, arguments.text().c_str(), nullptr);
}

} // namespace ear::pipewire
