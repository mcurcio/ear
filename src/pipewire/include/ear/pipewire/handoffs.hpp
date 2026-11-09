#pragma once
#include <memory>
#include <string_view>
#include "ear/core/executor.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/sink.hpp"
struct pw_loop;
// spa_ringbuffer + one pw_thread_loop per handoff, woken by pw_loop_signal_event.
namespace ear::pipewire {
class ThreadHandoffs final : public HandoffAdapter {
public:
	explicit ThreadHandoffs(pw_loop* mainLoop); // where `closed` runs; no Engine needed
	std::unique_ptr<Handoff> make(const NodeName& node, std::uint32_t channels, std::uint32_t capacityFrames,
	                              HandoffConsumer& consumer) override;

private:
	pw_loop* mainLoop_;
};
/// The interim sinks: ThreadHandoff's FrameRing, wake and pw_thread_loop, with a stamp ring beside the frame
/// ring, a consumer that begins on the sink's thread, and the realtime tap the port block writes after the code. Every
/// `begun` and `closed` is posted through `mainLoop`, the Executor NodeHost's InsidesEvents post through, so a consumer's
/// lost() said inside begin() lands first. Interim until slice 3b moves the ring behind a ring companion.
class ThreadSinks final : public SinkAdapter {
public:
	explicit ThreadSinks(Executor& mainLoop); // outlives every sink made
	std::unique_ptr<Sink> make(const NodeName& node, std::string_view bank, std::uint32_t channels,
	                           std::uint32_t capacityFrames) override;

private:
	Executor& mainLoop_;
};
} // namespace ear::pipewire
