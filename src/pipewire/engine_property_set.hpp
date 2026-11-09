// The only code that writes PipeWire properties or loads a module with arguments (docs/engine.md#properties-ear-sets):
// every key is a PropertyKey, which only the property table makes, and the property guard
// (test/pipewire/property_guard.cmake) refuses a call to PipeWire's property writers anywhere else. Private to
// ear_pipewire: its sources include it by this path; no public header does.
#pragma once

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "engine_properties.hpp"

struct pw_context;
struct pw_impl_module;
struct pw_properties;

namespace ear::pipewire {

using PropertyValue = std::pair<PropertyKey, std::string>;

/// New properties holding `values`, in order; the caller owns them (or hands them to PipeWire, which takes them).
pw_properties* newProperties(std::initializer_list<PropertyValue> values);

/// Sets one property.
void setProperty(pw_properties* properties, PropertyKey key, const std::string& value);

/// A module's arguments: an object of keys from the table, each value written as PipeWire's parser reads it (a quoted
/// string, a number, a word, an array or a nested object, already formatted by the caller).
class ModuleArguments {
public:
	ModuleArguments& add(PropertyKey key, std::string value);
	ModuleArguments& add(PropertyKey key, const ModuleArguments& object);
	/// "{ key = value ... }".
	std::string text() const;

private:
	std::vector<PropertyValue> values_;
};

/// pw_context_load_module with these arguments; null, with errno set, when it does not load.
pw_impl_module* loadModule(pw_context* context, const char* name, const ModuleArguments& arguments);

} // namespace ear::pipewire
