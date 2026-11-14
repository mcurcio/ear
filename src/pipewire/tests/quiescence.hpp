// The settled-engine wait (docs/testing.md#a-settled-engine). A test that reads what a change of plan or of links
// propagated (a latency retold from block to block, hop by hop) calls quiesce() before it reads, instead of waiting for
// one probe's sighting, which can come while the retell is still on its way to the next block. It takes anything with
// wake() and until() (either engine test file's Waiter), as probes.hpp's Probe does.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <string_view>
#include <vector>

#include <pipewire/pipewire.h>

#include "ear/pipewire/engine.hpp"

namespace ear::pipewire::testing {

/// Counts every port info that carries changed params, as the engine's own connection hears them through the
/// registry: a block's retell, the latency PipeWire sets on the block's peers in turn, a port that came or went. So it
/// sees the retells of every block, probe and companion, and ear_pipewire needs no counter of its own for a test.
template <class W> class Quiescence {
public:
	Quiescence(Engine& engine, W& waiter) : engine_(engine), waiter_(waiter) {
		registry_ = pw_core_get_registry(engine.core(), PW_VERSION_REGISTRY, 0);
		pw_registry_add_listener(registry_, &registryHook_, &registryEvents(), this);
		pw_core_add_listener(engine.core(), &coreHook_, &coreEvents(), this);
	}
	~Quiescence() {
		for (auto& [id, port] : ports_) unbind(port);
		spa_hook_remove(&coreHook_);
		spa_hook_remove(&registryHook_);
		pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
	}
	Quiescence(const Quiescence&) = delete;
	Quiescence& operator=(const Quiescence&) = delete;

	/// Port infos with changed params heard so far, and ports that came or went.
	std::uint64_t changes() const { return changes_; }

	/// pw_core_sync round trips until two in a row complete with no change (one quiet trip can end while PipeWire still
	/// holds work for a later turn of the main loop it shares with ear), then until each of `cycles` (a count of whole
	/// cycles a block's code ran) has moved on by two: one whole cycle of each begun after the engine settled, so its
	/// code read the settled latency. False past `deadline`, which only fails the test.
	bool quiesce(std::initializer_list<std::function<std::uint64_t()>> cycles,
	             std::chrono::seconds deadline = std::chrono::seconds(10)) {
		const auto end = std::chrono::steady_clock::now() + deadline;
		for (int quiet = 0; quiet < 2;) {
			if (std::chrono::steady_clock::now() > end) return false;
			const std::uint64_t before = changes_;
			const int seq = pw_core_sync(engine_.core(), PW_ID_CORE, 0);
			if (!waiter_.until([&] { return done_ == seq; }, deadline)) return false;
			quiet = changes_ == before ? quiet + 1 : 0;
			++roundTrips_;
		}
		std::vector<std::uint64_t> from;
		for (const auto& count : cycles) from.push_back(count());
		return waiter_.until(
			[&] {
				std::size_t i = 0;
				for (const auto& count : cycles)
					if (count() < from[i++] + 2) return false;
				return true;
			},
			deadline);
	}
	/// Round trips quiesce() has made, for a test's report.
	std::uint64_t roundTrips() const { return roundTrips_; }

private:
	struct Port {
		Quiescence* owner = nullptr;
		pw_proxy* proxy = nullptr;
		spa_hook hook{};
	};

	static const pw_registry_events& registryEvents() {
		static const pw_registry_events table = [] {
			pw_registry_events events{};
			events.version = PW_VERSION_REGISTRY_EVENTS;
			events.global = &Quiescence::onGlobal;
			events.global_remove = &Quiescence::onGlobalRemove;
			return events;
		}();
		return table;
	}
	static const pw_core_events& coreEvents() {
		static const pw_core_events table = [] {
			pw_core_events events{};
			events.version = PW_VERSION_CORE_EVENTS;
			events.done = &Quiescence::onDone;
			return events;
		}();
		return table;
	}
	static const pw_port_events& portEvents() {
		static const pw_port_events table = [] {
			pw_port_events events{};
			events.version = PW_VERSION_PORT_EVENTS;
			events.info = &Quiescence::onPortInfo;
			return events;
		}();
		return table;
	}

	static void onGlobal(void* data, std::uint32_t id, std::uint32_t, const char* type, std::uint32_t,
	                     const spa_dict*) {
		auto* self = static_cast<Quiescence*>(data);
		if (std::string_view(type) != PW_TYPE_INTERFACE_Port) return;
		Port& port = self->ports_[id];
		port.owner = self;
		port.proxy =
			static_cast<pw_proxy*>(pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Port, PW_VERSION_PORT, 0));
		if (port.proxy != nullptr) pw_proxy_add_object_listener(port.proxy, &port.hook, &portEvents(), &port);
		++self->changes_;
		self->waiter_.wake();
	}
	static void onGlobalRemove(void* data, std::uint32_t id) {
		auto* self = static_cast<Quiescence*>(data);
		const auto found = self->ports_.find(id);
		if (found == self->ports_.end()) return;
		unbind(found->second);
		self->ports_.erase(found);
		++self->changes_;
		self->waiter_.wake();
	}
	static void onPortInfo(void* data, const pw_port_info* info) {
		auto* port = static_cast<Port*>(data);
		if ((info->change_mask & PW_PORT_CHANGE_MASK_PARAMS) == 0) return;
		++port->owner->changes_;
		port->owner->waiter_.wake();
	}
	static void onDone(void* data, std::uint32_t id, int seq) {
		auto* self = static_cast<Quiescence*>(data);
		if (id != PW_ID_CORE) return;
		self->done_ = seq;
		self->waiter_.wake();
	}
	static void unbind(Port& port) {
		if (port.proxy == nullptr) return;
		spa_hook_remove(&port.hook);
		pw_proxy_destroy(port.proxy);
		port.proxy = nullptr;
	}

	Engine& engine_;
	W& waiter_;
	pw_registry* registry_ = nullptr;
	spa_hook registryHook_{};
	spa_hook coreHook_{};
	std::map<std::uint32_t, Port> ports_; // a map's elements stay put: their hooks are registered
	std::uint64_t changes_ = 0;
	std::uint64_t roundTrips_ = 0;
	int done_ = -1;
};

} // namespace ear::pipewire::testing
