#pragma once
#include <memory>
#include <string_view>
#include "ear/core/executor.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/sink.hpp"
namespace ear::pipewire {
/// The sinks (docs/engine.md#sinks), ring companions their block taps (docs/architecture.md#companions): one
/// pw_thread_loop per sink, woken by pw_loop_signal_event, around core's SinkRing (a frame ring with a stamp ring
/// beside it), a consumer that begins on the sink's thread, and the realtime tap the port block writes after the code. Every `begun` and `closed` is
/// posted through `mainLoop`, the Executor a node's handle posts through, so a consumer's lost() said inside
/// begin() lands first.
class ThreadSinks final : public SinkAdapter {
public:
	explicit ThreadSinks(Executor& mainLoop); // outlives every sink made
	std::unique_ptr<Sink> make(const NodeName& node, std::string_view bank, std::uint32_t channels,
	                           std::uint32_t capacityFrames, SampleRate rate) override;

private:
	Executor& mainLoop_;
};
} // namespace ear::pipewire
