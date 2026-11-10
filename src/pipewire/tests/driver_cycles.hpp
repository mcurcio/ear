// DriverCycles: the graph's cycles as the internal clock's driver runs them, and which it cut short
// (docs/testing.md#making-the-rare-event). Its own header, so a test with a harness of its own (port_block_test.cpp)
// uses it too; engine_harness.hpp includes it.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <pipewire/impl.h>
#include <pipewire/pipewire.h>

#include "catch2/catch.hpp"
#include "ear/pipewire/engine.hpp"

namespace ear::pipewire::testing {

/// The graph's cycles as the internal clock's driver (Dummy-Driver, an engine node in this process) runs them: each
/// cycle it starts, and each it finds cut short as the next begins (PipeWire's realtime `incomplete` event: a node had
/// not finished, so the nodes after it lost that cycle; docs/engine.md#the-one-data-loop-and-invokes). It stands in for
/// a ledger where the last node of a chain runs no code of ear's (a card): a code upstream reads started() in its
/// process() to name the cycle it ran in, and cutShort() of that says whether the nodes after it may have missed it.
/// Cycle 0 is every cycle before this listened.
class DriverCycles {
public:
	DriverCycles(Engine& engine, std::size_t capacity) : cut_(capacity) {
		struct Search {
			pw_impl_node* found = nullptr;
		} search;
		pw_context_for_each_global(
			engine.context(),
			[](void* data, pw_global* global) {
				if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Node)) return 0;
				auto* node = static_cast<pw_impl_node*>(pw_global_get_object(global));
				const char* name = pw_properties_get(pw_impl_node_get_properties(node), PW_KEY_NODE_NAME);
				if (name == nullptr || std::string_view(name) != "Dummy-Driver") return 0;
				static_cast<Search*>(data)->found = node;
				return 1;
			},
			&search);
		if (search.found == nullptr) FAIL("the engine holds no node named Dummy-Driver");
		node_ = search.found;
		events_.version = PW_VERSION_IMPL_NODE_RT_EVENTS;
		events_.start = &DriverCycles::onStart;
		events_.incomplete = &DriverCycles::onIncomplete;
		pw_impl_node_add_rt_listener(node_, &hook_, &events_, this); // taken up on the data thread
	}
	DriverCycles(const DriverCycles&) = delete;
	DriverCycles& operator=(const DriverCycles&) = delete;
	~DriverCycles() { pw_impl_node_remove_rt_listener(node_, &hook_); } // returns once the data thread let go

	/// Cycles started since this listened; inside a process(), the cycle it runs in. Any thread.
	std::uint64_t started() const noexcept { return started_.load(std::memory_order_acquire); }
	/// Cycle `cycle` (from started()) was cut short: false for cycle 0 and for one still running, whose cut is told as
	/// the next one starts. Any thread.
	bool cutShort(std::uint64_t cycle) const noexcept {
		return cycle > 0 && cycle <= cut_.size() && cut_[cycle - 1].load(std::memory_order_acquire);
	}
	/// Cycles cut short so far, and whether one started past the capacity (its cut is not kept).
	std::uint64_t cuts() const noexcept { return cuts_.load(std::memory_order_acquire); }
	bool overflowed() const noexcept { return started() > cut_.size(); }

private:
	// Data thread: incomplete is told in the next cycle's start, before that cycle's start event.
	static void onStart(void* data) {
		auto* self = static_cast<DriverCycles*>(data);
		self->started_.fetch_add(1, std::memory_order_release);
	}
	static void onIncomplete(void* data) {
		auto* self = static_cast<DriverCycles*>(data);
		const std::uint64_t cycle = self->started_.load(std::memory_order_relaxed); // the one cut short
		if (cycle > 0 && cycle <= self->cut_.size()) self->cut_[cycle - 1].store(true, std::memory_order_release);
		self->cuts_.fetch_add(1, std::memory_order_release);
	}

	pw_impl_node* node_ = nullptr;
	pw_impl_node_rt_events events_{};
	spa_hook hook_{};
	std::vector<std::atomic<bool>> cut_;
	std::atomic<std::uint64_t> started_{0};
	std::atomic<std::uint64_t> cuts_{0};
};

} // namespace ear::pipewire::testing
