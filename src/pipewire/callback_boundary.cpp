// The engine's callback boundary (callback_boundary.hpp, docs/architecture.md#the-callback-boundary): where a caught
// exception is reported. The engine installs its Faults on the main thread; a trampoline reads it only when it caught
// something, from whichever thread it ran on.
#include "callback_boundary.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

#include <pipewire/impl.h>

#include "ear/core/main_loop_activity.hpp"

namespace ear::pipewire::boundary {
namespace {

std::atomic<bool> engaged{false}; // an engine (or a test) holds the boundary
std::atomic<Faults*> installed{nullptr};
std::atomic<std::thread::id> mainThread{};
std::atomic<bool> wedged{false};       // the data loop runs no invoke
std::atomic<std::size_t> heldNow{0};   // registrations held
std::atomic<std::size_t> throwAt{0};   // the seam: the registration that throws once held, counted down; 0 none
std::atomic<std::size_t> keptCount{0}; // realtime listeners left on a wedged data loop

/// With no Faults installed (an engine made without one): today's outcome, the process ends, with a line naming the
/// callback first among the crash's last words.
class LastWords final : public Faults {
public:
	void caught(std::string_view callback, std::string_view where, std::string_view detail, std::string_view /*node*/,
	            bool /*owned*/) noexcept override {
		const std::string line = faultLine(callback, where, detail);
		std::fwrite(line.data(), 1, line.size(), stderr);
		std::fflush(stderr);
		std::terminate(); // inside the handler: terminate's own message still names what was thrown
	}
};

bool onMainThread() noexcept { return std::this_thread::get_id() == mainThread.load(std::memory_order_acquire); }

/// What the exception being handled says.
std::string detailOfCaught() {
	try {
		throw;
	} catch (const std::exception& error) {
		return error.what();
	} catch (...) {
		return "an exception that is no std::exception";
	}
}

} // namespace

bool install(Faults* faults) noexcept {
	if (engaged.exchange(true, std::memory_order_acq_rel)) return false;
	wedged.store(false, std::memory_order_release);
	mainThread.store(std::this_thread::get_id(), std::memory_order_release);
	installed.store(faults, std::memory_order_release);
	return true;
}

void uninstall() noexcept {
	wedged.store(false, std::memory_order_release);
	installed.store(nullptr, std::memory_order_release);
	mainThread.store(std::thread::id{}, std::memory_order_release);
	engaged.store(false, std::memory_order_release);
}

namespace {
std::atomic<bool> callbackArmed{false}; // read by every guarded callback: the seam's string only when armed
std::string armedCallback;              // main thread
} // namespace

void throwInCallback(std::string_view callback) {
	armedCallback = std::string(callback);
	callbackArmed.store(!armedCallback.empty(), std::memory_order_release);
}

void injectedIn(std::string_view callback) {
	if (!callbackArmed.load(std::memory_order_acquire) || !onMainThread() || armedCallback != callback) return;
	callbackArmed.store(false, std::memory_order_release);
	armedCallback.clear();
	throw std::runtime_error("injected");
}

void markDataLoopWedged() noexcept { wedged.store(true, std::memory_order_release); }

bool dataLoopWedged() noexcept { return wedged.load(std::memory_order_acquire); }

void clearDataLoopWedged() noexcept { wedged.store(false, std::memory_order_release); }

void throwAfterRegistration(std::size_t n) noexcept { throwAt.store(n, std::memory_order_release); }

std::size_t registrationsHeld() noexcept { return heldNow.load(std::memory_order_acquire); }

namespace {
// What keptForever took, reachable for the process's life (a leak checker sees them held, not lost); past its room a
// left listener is still never freed, only no longer listed.
std::array<std::atomic<void*>, 64> kept{};
} // namespace

void keptForever(void* left) noexcept {
	const std::size_t at = keptCount.fetch_add(1, std::memory_order_acq_rel);
	if (at < kept.size()) kept[at].store(left, std::memory_order_release);
}

std::size_t keptForeverCount() noexcept { return keptCount.load(std::memory_order_acquire); }

void held() {
	heldNow.fetch_add(1, std::memory_order_acq_rel);
	std::size_t at = throwAt.load(std::memory_order_acquire);
	while (at != 0 && !throwAt.compare_exchange_weak(at, at - 1, std::memory_order_acq_rel)) {
	}
	if (at == 1) throw std::runtime_error("injected after a registration");
}

void released() noexcept { heldNow.fetch_sub(1, std::memory_order_acq_rel); }

void entered() noexcept {
	// A callback made while an exception unwinds (pw_filter_destroy, in a destructor, telling a state change) must not
	// forget the place that exception is carrying.
	if (std::uncaught_exceptions() == 0 && onMainThread()) MainLoopActivity::forgetThrown();
}

void report(std::string_view callback, FaultOwner* owner) noexcept {
	const std::string detail = detailOfCaught();
	std::string where;
	if (onMainThread()) {
		// The place the throw left; else the call still open around this callback (one PipeWire made inside it). Only
		// core's dispatchers leave a place (a scope a throw ended, or NodeHost's rethrow putting one back), so a throw
		// that carries one came out of core, whose state is every node's: its bug, whichever adapter's callback it came
		// out of (docs/architecture.md#faults-by-owner).
		const auto at = MainLoopActivity::takeThrown();
		if (at) owner = nullptr;
		if (owner != nullptr && !owner->firstFault()) return; // its node already fails: said once
		where = MainLoopActivity::describe(at ? *at : MainLoopActivity::save());
	}
	static LastWords lastWords;
	Faults* faults = installed.load(std::memory_order_acquire);
	const std::string_view node = owner != nullptr ? owner->faultNode() : std::string_view{};
	(faults != nullptr ? *faults : lastWords).caught(callback, where, detail, node, owner != nullptr);
	if (owner == nullptr) return;
	try {
		owner->faulted(callback, detail);
	} catch (...) {
		report(callback, nullptr); // NodeHost's bug as the owner told it: it carries NodeHost's place
	}
}

} // namespace ear::pipewire::boundary

namespace ear::pipewire {

namespace {
struct NodeSearch {
	std::string_view name;
	pw_impl_node* found = nullptr;
};

/// PipeWire calls it for each global, on the calling thread: nothing here throws.
int matchNode(void* data, pw_global* global) noexcept {
	if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Node)) return 0;
	auto* search = static_cast<NodeSearch*>(data);
	auto* node = static_cast<pw_impl_node*>(pw_global_get_object(global));
	const char* name = pw_properties_get(pw_impl_node_get_properties(node), PW_KEY_NODE_NAME);
	if (name == nullptr || std::string_view(name) != search->name) return 0;
	search->found = node;
	return 1;
}
} // namespace

pw_impl_node* engineNode(pw_context* context, std::string_view name) noexcept {
	NodeSearch search{.name = name};
	pw_context_for_each_global(context, &matchNode, &search);
	return search.found;
}

void RecordThenEnd::caught(std::string_view callback, std::string_view where, std::string_view detail,
                           std::string_view node, bool owned) noexcept {
	try {
		record_(InternalError{.callback = callback, .node = node, .where = where, .what = detail, .fatal = !owned});
	} catch (...) { // no memory for the record: the line below still says it, or the node's failure does
	}
	if (owned) return;
	try {
		lastWords_(faultLine(callback, where, detail));
	} catch (...) {
	}
	std::terminate(); // inside the handler: terminate's own message still names what was thrown
}

std::string faultLine(std::string_view callback, std::string_view where, std::string_view detail) {
	std::string line = "ear: internal error in ";
	line += callback;
	if (!where.empty()) {
		line += ", during ";
		line += where;
	}
	line += ": ";
	line += detail;
	line += '\n';
	return line;
}

} // namespace ear::pipewire
