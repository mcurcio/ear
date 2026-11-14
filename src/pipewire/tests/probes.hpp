// Test-only probes (docs/engine.md#the-one-data-loop-and-invokes): bare pw_filters beside the port blocks, each
// running a ProbeCode on the data thread, so a test sees the graph from outside the code under test. Moved out of
// spikes_test.cpp for port_block_test.cpp. A probe takes anything with wake() (either test
// file's Waiter), called on the main loop at each of its state changes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

#include "catch2/catch.hpp"
#include "ear/pipewire/engine.hpp"

namespace ear::pipewire::testing {

/// What a probe runs each cycle, on the data thread.
class ProbeCode {
public:
	virtual void cycle(const spa_io_clock& clock, std::span<const float* const> inputs, std::span<float* const> outputs,
	                   std::uint32_t frames) noexcept = 0;

protected:
	~ProbeCode() = default;
};

/// A bare pw_filter as a port block is one (port_blocks.cpp): in ear.graph (engine_names.hpp's graphGroup), always
/// processing, RT_PROCESS, one 32-bit float mono port per name ("in.<i>", "out.<i>"). An input with no buffer this cycle
/// reads zeros; an output with none writes to scratch.
class Probe {
public:
	template <class W>
	Probe(Engine& engine, W& waiter, const char* name, std::size_t inputs, std::size_t outputs, ProbeCode& code)
		: Probe(engine, std::function<void()>([&waiter] { waiter.wake(); }), name, inputs, outputs, code) {}
	Probe(Engine& engine, std::function<void()> wake, const char* name, std::size_t inputs, std::size_t outputs,
	      ProbeCode& code)
		: wake_(std::move(wake)), code_(code), zeros_(8192, 0.0F), scratch_(8192, 0.0F), inputs_(inputs),
		  outputs_(outputs) {
		filter_ = pw_filter_new(engine.core(), name,
		                        pw_properties_new(PW_KEY_NODE_NAME, name, PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_NODE_GROUP,
		                                          "ear.graph", PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr));
		REQUIRE(filter_ != nullptr);
		pw_filter_add_listener(filter_, &listener_, &events(), this);
		for (std::size_t i = 0; i < inputs; ++i) inPorts_.push_back(addPort(PW_DIRECTION_INPUT, "in", i + 1));
		for (std::size_t i = 0; i < outputs; ++i) outPorts_.push_back(addPort(PW_DIRECTION_OUTPUT, "out", i + 1));
		REQUIRE(pw_filter_connect(filter_, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) >= 0);
	}
	~Probe() {
		spa_hook_remove(&listener_);
		pw_filter_destroy(filter_);
	}
	Probe(const Probe&) = delete;
	Probe& operator=(const Probe&) = delete;

	bool streaming() const { return streaming_; }
	pw_loop* dataLoop() const { return pw_filter_get_data_loop(filter_); }
	/// The probe's engine node's global id (pw_filter_get_node_id), once bound.
	std::uint32_t nodeId() const { return pw_filter_get_node_id(filter_); }
	/// Takes the probe out of the graph (false) or puts it back (true): pw_filter_set_active. S6 stops the internal
	/// clock this way, when the probe is the only node it drives.
	void setActive(bool active) { REQUIRE(pw_filter_set_active(filter_, active) >= 0); }

private:
	void* addPort(pw_direction direction, const char* bank, std::size_t member) {
		const std::string name = std::string(bank) + "." + std::to_string(member);
		return pw_filter_add_port(
			filter_, direction, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
			pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, name.c_str(), nullptr),
			nullptr, 0);
	}
	static const pw_filter_events& events() {
		static const pw_filter_events table = [] {
			pw_filter_events events{};
			events.version = PW_VERSION_FILTER_EVENTS;
			events.state_changed = &Probe::onStateChanged;
			events.process = &Probe::onProcess;
			return events;
		}();
		return table;
	}
	static void onStateChanged(void* data, pw_filter_state, pw_filter_state state, const char*) {
		auto* self = static_cast<Probe*>(data);
		if (state == PW_FILTER_STATE_STREAMING) self->streaming_ = true;
		self->wake_();
	}
	static void onProcess(void* data, spa_io_position* position) {
		auto* self = static_cast<Probe*>(data);
		const auto frames = static_cast<std::uint32_t>(position->clock.duration);
		if (frames > self->zeros_.size()) return;
		for (std::size_t i = 0; i < self->inPorts_.size(); ++i) {
			const auto* in = static_cast<const float*>(pw_filter_get_dsp_buffer(self->inPorts_[i], frames));
			self->inputs_[i] = in != nullptr ? in : self->zeros_.data();
		}
		for (std::size_t i = 0; i < self->outPorts_.size(); ++i) {
			auto* out = static_cast<float*>(pw_filter_get_dsp_buffer(self->outPorts_[i], frames));
			self->outputs_[i] = out != nullptr ? out : self->scratch_.data();
		}
		self->code_.cycle(position->clock, self->inputs_, self->outputs_, frames);
	}

	std::function<void()> wake_;
	ProbeCode& code_;
	std::vector<float> zeros_;
	std::vector<float> scratch_;
	std::vector<const float*> inputs_;
	std::vector<float*> outputs_;
	std::vector<void*> inPorts_;
	std::vector<void*> outPorts_;
	pw_filter* filter_ = nullptr;
	spa_hook listener_{};
	bool streaming_ = false;
};

} // namespace ear::pipewire::testing
