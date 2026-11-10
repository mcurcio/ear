// Where an exception caught at the engine's callback boundary is reported (docs/architecture.md#the-callback-boundary).
// Every C callback ear gives PipeWire is registered through that boundary (ear_pipewire's private
// callback_boundary.hpp), which catches what its member threw and tells the Faults the engine was made with. main.cpp
// provides it; an engine made with none ends the process the same way (EngineSettings::faults).
#pragma once
#include <string>
#include <string_view>
namespace ear::pipewire {
class Faults {
public:
	/// Called on the thread the callback ran on (the main loop, or a sink's or legacy handoff's thread), inside the
	/// handler of the exception it caught, so a std::terminate() here still says what was thrown. `callback` names the
	/// member PipeWire called ("FilterPortBlock::onStateChanged"), or the posted item or timer for the main-loop
	/// executor's own; `where` is what the main loop was in when the exception was thrown (MainLoopActivity's "<call>
	/// <node>", such as "make block hall"), empty off the main thread or when it was in no call; `detail` is what it
	/// said. `owned`: the callback's object is an adapter whose state is one node's (`node`, empty for a route's link),
	/// told next so only that node fails, and this must return; otherwise the fault is ear's own with no owner, and
	/// the process must end (docs/architecture.md#faults-by-owner).
	virtual void caught(std::string_view callback, std::string_view where, std::string_view detail,
	                    std::string_view node, bool owned) noexcept = 0;

protected:
	~Faults() = default;
};
/// The crash's last line for a fault: "ear: internal error in <callback>[, during <where>]: <detail>", with its
/// newline. One wording for main.cpp's Faults and the boundary's own.
std::string faultLine(std::string_view callback, std::string_view where, std::string_view detail);
} // namespace ear::pipewire
