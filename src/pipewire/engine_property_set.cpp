// The only calls to PipeWire's property writers and module loading with arguments (engine_property_set.hpp,
// docs/engine.md#properties-ear-sets).
#include "engine_property_set.hpp"

#include <pipewire/impl.h>
#include <pipewire/pipewire.h>

namespace ear::pipewire {

pw_properties* newProperties(std::initializer_list<PropertyValue> values) {
	pw_properties* properties = pw_properties_new(nullptr, nullptr);
	for (const auto& [key, value] : values) setProperty(properties, key, value);
	return properties;
}

void setProperty(pw_properties* properties, PropertyKey key, const std::string& value) {
	pw_properties_set(properties, key.c_str(), value.c_str());
}

ModuleArguments& ModuleArguments::add(PropertyKey key, std::string value) {
	values_.emplace_back(key, std::move(value));
	return *this;
}

ModuleArguments& ModuleArguments::add(PropertyKey key, const ModuleArguments& object) {
	return add(key, object.text());
}

std::string ModuleArguments::text() const {
	std::string text = "{";
	for (const auto& [key, value] : values_) text += std::string(" ") + key.c_str() + " = " + value;
	return text + " }";
}

pw_impl_module* loadModule(pw_context* context, const char* name, const ModuleArguments& arguments) {
	return pw_context_load_module(context, name, arguments.text().c_str(), nullptr);
}

} // namespace ear::pipewire
