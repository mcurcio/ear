// The capture (upstream) latency one port tells, as any client reads it (docs/engine.md#latency-per-bank-pair): shared
// by the tests that check what a block or a module-run converter's port tells (spikes_test.cpp's S7, the RTP and pipe
// receivers' latency tests).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <pipewire/pipewire.h>
#include <spa/param/latency-utils.h>

#include "engine_harness.hpp"

namespace ear::pipewire::testing {

/// The capture (upstream) latency one port tells, as any client reads it: a registry binding finds the port by its
/// node's global id and its name, binds it, and keeps its newest Latency param of direction output.
class PortLatency {
public:
	PortLatency(Engine& engine, Waiter& waiter, std::uint32_t nodeId, std::string portName)
		: waiter_(waiter), nodeId_(nodeId), portName_(std::move(portName)) {
		registry_ = pw_core_get_registry(engine.core(), PW_VERSION_REGISTRY, 0);
		pw_registry_add_listener(registry_, &registryHook_, &registryEvents(), this);
	}
	~PortLatency() {
		if (port_ != nullptr) {
			spa_hook_remove(&portHook_);
			pw_proxy_destroy(port_);
		}
		spa_hook_remove(&registryHook_);
		pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
	}
	PortLatency(const PortLatency&) = delete;
	PortLatency& operator=(const PortLatency&) = delete;

	std::optional<Latency> told() const { return told_; }
	std::size_t tellings() const { return tellings_; }

private:
	static const pw_registry_events& registryEvents() {
		static const pw_registry_events table = [] {
			pw_registry_events events{};
			events.version = PW_VERSION_REGISTRY_EVENTS;
			events.global = &PortLatency::onGlobal;
			return events;
		}();
		return table;
	}
	static const pw_port_events& portEvents() {
		static const pw_port_events table = [] {
			pw_port_events events{};
			events.version = PW_VERSION_PORT_EVENTS;
			events.param = &PortLatency::onParam;
			return events;
		}();
		return table;
	}
	static void onGlobal(void* data, std::uint32_t id, std::uint32_t /*permissions*/, const char* type,
	                     std::uint32_t /*version*/, const spa_dict* props) {
		auto* self = static_cast<PortLatency*>(data);
		if (self->port_ != nullptr || std::string_view(type) != PW_TYPE_INTERFACE_Port || props == nullptr) return;
		const char* node = spa_dict_lookup(props, PW_KEY_NODE_ID);
		const char* name = spa_dict_lookup(props, PW_KEY_PORT_NAME);
		if (node == nullptr || name == nullptr || std::to_string(self->nodeId_) != node || self->portName_ != name)
			return;
		self->port_ =
			static_cast<pw_proxy*>(pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Port, PW_VERSION_PORT, 0));
		pw_proxy_add_object_listener(self->port_, &self->portHook_, &portEvents(), self);
		std::uint32_t wanted = SPA_PARAM_Latency;
		pw_port_subscribe_params(reinterpret_cast<pw_port*>(self->port_), &wanted, 1);
	}
	static void onParam(void* data, int /*seq*/, std::uint32_t id, std::uint32_t /*index*/, std::uint32_t /*next*/,
	                    const spa_pod* param) {
		auto* self = static_cast<PortLatency*>(data);
		spa_latency_info info{};
		if (id != SPA_PARAM_Latency || param == nullptr || spa_latency_parse(param, &info) < 0 ||
		    info.direction != SPA_DIRECTION_OUTPUT)
			return;
		self->told_ = Latency{static_cast<std::uint32_t>(info.min_rate), static_cast<std::uint32_t>(info.max_rate)};
		++self->tellings_;
		self->waiter_.wake();
	}

	Waiter& waiter_;
	std::uint32_t nodeId_;
	std::string portName_;
	pw_registry* registry_ = nullptr;
	spa_hook registryHook_{};
	pw_proxy* port_ = nullptr;
	spa_hook portHook_{};
	std::optional<Latency> told_;
	std::size_t tellings_ = 0;
};

} // namespace ear::pipewire::testing
