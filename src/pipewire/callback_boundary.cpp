// The engine's callback boundary (callback_boundary.hpp, docs/architecture.md#the-callback-boundary): where a caught
// exception is reported. The engine installs its Faults on the main thread; a trampoline reads it only when it caught
// something, from whichever thread it ran on.
#include "callback_boundary.hpp"

#include <atomic>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>

#include "ear/core/main_loop_activity.hpp"

namespace ear::pipewire::boundary {
namespace {

std::atomic<bool> engaged{false}; // an engine (or a test) holds the boundary
std::atomic<Faults*> installed{nullptr};
std::atomic<std::thread::id> mainThread{};

/// With no Faults installed (an engine made without one): today's outcome, the process ends, with a line naming the
/// callback first among the crash's last words.
class LastWords final : public Faults {
public:
	void caught(std::string_view callback, std::string_view where, std::string_view detail,
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
	mainThread.store(std::this_thread::get_id(), std::memory_order_release);
	installed.store(faults, std::memory_order_release);
	return true;
}

void uninstall() noexcept {
	installed.store(nullptr, std::memory_order_release);
	mainThread.store(std::thread::id{}, std::memory_order_release);
	engaged.store(false, std::memory_order_release);
}

void entered() noexcept {
	// A callback made while an exception unwinds (pw_filter_destroy, in a destructor, telling a state change) must not
	// forget the place that exception is carrying.
	if (std::uncaught_exceptions() == 0 && onMainThread()) MainLoopActivity::forgetThrown();
}

void report(std::string_view callback, FaultOwner* owner) noexcept {
	const std::string detail = detailOfCaught();
	std::string where;
	if (onMainThread()) {
		// The place the throw left; else the call still open around this callback (one PipeWire made inside it).
		const auto at = MainLoopActivity::takeThrown();
		where = MainLoopActivity::describe(at ? *at : MainLoopActivity::save());
	}
	static LastWords lastWords;
	Faults* faults = installed.load(std::memory_order_acquire);
	(faults != nullptr ? *faults : lastWords).caught(callback, where, detail, owner != nullptr);
	if (owner != nullptr) owner->faulted(callback, detail);
}

} // namespace ear::pipewire::boundary

namespace ear::pipewire {

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
