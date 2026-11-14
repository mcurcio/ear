// The only code that writes PipeWire properties, makes a core object from them or loads a module with arguments
// (docs/engine.md#properties-ear-sets). Every key is a PropertyKey<H>, which only the property table makes for home H,
// and every writer takes only the keys of its properties' own home, so a key of one home never lands on another's
// object. The property guard (test/pipewire/property_guard.cmake) refuses PipeWire's property writers, its
// pw_core_create_object and its module loading anywhere else. Private to ear_pipewire: its sources include it by this
// path; no public header does.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>

#include "engine_properties.hpp"

struct pw_impl_module;

namespace ear::pipewire {

/// std::logic_error unless `value` is one value of a module's arguments: not empty, and no whitespace outside quotes,
/// an array or an object (which would end the value and open another key).
void requireOneValue(const std::string& value);

template <Home H> using PropertyValue = std::pair<PropertyKey<H>, std::string>;

/// Properties of one object of home H, owned until released to PipeWire.
template <Home H> class PropertySet {
public:
	explicit PropertySet(std::initializer_list<PropertyValue<H>> values) : raw_(pw_properties_new(nullptr, nullptr)) {
		for (const auto& [key, value] : values) set(key, value);
	}
	~PropertySet() {
		if (raw_ != nullptr) pw_properties_free(raw_);
	}
	PropertySet(const PropertySet&) = delete;
	PropertySet& operator=(const PropertySet&) = delete;

	/// Sets one property of this home.
	void set(PropertyKey<H> key, const std::string& value) { pw_properties_set(raw_, key.c_str(), value.c_str()); }
	/// A card's properties also take its test seam's keys, and no other home's (seamChecked, engine_properties.hpp).
	void set(PropertyKey<Home::cardSeam> key, const std::string& value)
		requires(H == Home::card)
	{
		pw_properties_set(raw_, key.c_str(), value.c_str());
	}
	/// Hands the properties to a PipeWire call that takes them (pw_context_new, pw_filter_new, pw_filter_add_port).
	pw_properties* release() noexcept { return std::exchange(raw_, nullptr); }

	template <Home Object> friend pw_proxy* createObject(pw_core* core, const char* factory, const char* type,
	                                                     std::uint32_t version, const PropertySet<Object>& properties);

private:
	pw_properties* raw_;
};

/// pw_core_create_object with these properties; null, with errno set, when it cannot be asked.
template <Home H> pw_proxy* createObject(pw_core* core, const char* factory, const char* type, std::uint32_t version,
                                         const PropertySet<H>& properties) {
	return static_cast<pw_proxy*>(pw_core_create_object(core, factory, type, version, &properties.raw_->dict, 0));
}

/// A module's arguments for an object of home H: an object of that home's keys, each value as PipeWire's parser reads
/// it (a quoted string, a number, a word, an array or a nested object, already formatted by the caller). A value that
/// would open another key (unquoted whitespace outside an array or object, or nothing at all) is refused with
/// std::logic_error.
template <Home H> class ModuleArguments {
public:
	ModuleArguments& add(PropertyKey<H> key, std::string value) {
		requireOneValue(value);
		values_.emplace_back(key, std::move(value));
		return *this;
	}
	/// The receiver's stream.props: the RTP stream's own arguments, nested.
	ModuleArguments& add(PropertyKey<Home::rtpReceiver> key, const ModuleArguments<Home::rtpStream>& stream)
		requires(H == Home::rtpReceiver)
	{
		if (key.view() != "stream.props") throw std::logic_error("only stream.props holds the RTP stream's arguments");
		return add(key, stream.text());
	}
	/// "{ key = value ... }".
	std::string text() const {
		std::string text = "{";
		for (const auto& [key, value] : values_) text += std::string(" ") + key.c_str() + " = " + value;
		return text + " }";
	}

private:
	std::vector<PropertyValue<H>> values_;
};

/// pw_context_load_module with an RTP receiver's arguments; null, with errno set, when it does not load.
pw_impl_module* loadModule(pw_context* context, const char* name, const ModuleArguments<Home::rtpReceiver>& arguments);

} // namespace ear::pipewire
