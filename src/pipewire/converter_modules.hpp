// What one module-run converter's module is (docs/engine.md#module-run-converters): the pieces that are a module's own,
// which the shared body (module_converter_body.hpp) reads and nothing else. Private to ear_pipewire: its sources and
// tests include it by this path; no public header does.
#pragma once

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>

#include "ear/core/companions.hpp"
#include "engine_companions.hpp"

struct pw_context;
struct pw_impl_module;

namespace ear::pipewire {

/// A module's own words and ports. Every member names the module and nothing of ear's.
struct ConverterModule {
	std::string_view kind; // "RTP receiver": names it in loss details, as every engine kind's word does
	std::string_view word; // "rtp-source": "cannot load PipeWire's <word> module"
	std::string_view body; // "RtpReceiverBody": how fault reports name the body's callbacks (NamesItself)
	ChannelFlow flow;      // nodeToBlock: a receiver feeds an inside input bank
	std::string (*nodePort)(std::size_t channel); // the module node's port for channel `channel` (from 0)
};

/// Per module-run shape, defined beside its adapter's instantiation (an explicit specialization):
///   static constexpr ConverterModule module;
///   using Arguments = ...;  // the module's arguments, built only from parsed values
///   static Arguments arguments(const NodeName& engineNode, const Shape& shape, std::size_t channels);
///   static pw_impl_module* load(pw_context* context, const Arguments& arguments); // null, errno set, when it does not
/// A shipped module loads through engine_property_set.hpp's loadModule, the only module loading the property guard
/// lets ear_pipewire's sources do; a test's stand-in, under a tests folder, loads its own.
template <class Shape> struct ModuleRun;

/// A shape whose ModuleRun is complete.
template <class Shape>
concept DescribedModuleRun = requires(const NodeName& engineNode, const Shape& shape, std::size_t channels,
                                      pw_context* context, const typename ModuleRun<Shape>::Arguments& arguments) {
	{ ModuleRun<Shape>::module } -> std::convertible_to<ConverterModule>;
	{ ModuleRun<Shape>::arguments(engineNode, shape, channels) } -> std::same_as<typename ModuleRun<Shape>::Arguments>;
	{ ModuleRun<Shape>::load(context, arguments) } -> std::same_as<pw_impl_module*>;
};

} // namespace ear::pipewire
