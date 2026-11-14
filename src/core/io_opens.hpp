// A resource a configuration opens on the I/O thread at readiness step `opens` (docs/engine.md#readiness-deadlines): a
// store (docs/writing-a-node-type.md#stores) or a recording (docs/writing-a-node-type.md#recordings). The pattern both
// share, in one place: the open is one job holding only the resource's shared state; what it found (opened, refused,
// or the adapter's bug) rides back to the main loop through the configuration's Completions (postHolding), so once the
// configuration ended nothing lands and the state's last reference is released on the I/O thread; its readiness item
// has the kind's bound. What an open does, and what a landing tells the node, are the caller's. Private to ear_core.
#pragma once
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "completions.hpp"
#include "ear/core/io_executor.hpp"
#include "readiness.hpp"

namespace ear::detail {

/// An adapter's bug caught in an I/O job: the adapter call that failed (its callback, as ear.internal-error names it)
/// and what it said. The node fails lost (docs/architecture.md#faults-by-owner).
struct IoFault {
	std::string callback;
	std::string what;
};

/// What an open found: its outcome (a refusal is an error, the node's start-failed), or the adapter's bug.
struct IoOpened {
	std::expected<void, std::string> outcome{};
	std::optional<IoFault> fault{};
};

/// Posts `open(*state)` to `io` as the job `job`. `open` runs on the I/O thread and never throws: it catches and sorts
/// what its adapter throws itself (a bug into `fault`, a refusal into `outcome`). Its result lands as
/// `landed(outcome, fault)` on the main loop through `completions`, or, once the configuration ended, never; then the
/// state's reference the completion holds goes back to `io` as the job `release`. `io` outlives what is queued.
template <class State, class Open, class Landed>
void postIoOpen(IoExecutor& io, std::string job, std::string release, std::shared_ptr<State> state,
                std::shared_ptr<Completions> completions, Open open, Landed landed) {
	io.post(std::move(job), [state = std::move(state), completions = std::move(completions), io = &io,
	                         release = std::move(release), open = std::move(open),
	                         landed = std::move(landed)]() mutable {
		struct Held {
			std::shared_ptr<State> state; // the completion's handle: dropped, released on the I/O thread
			IoOpened found;
		};
		auto held = std::make_shared<Held>();
		held->found = open(*state);
		held->state = std::move(state);
		completions->template postHolding<Held>(std::move(held), *io, std::move(release),
		                                        [landed = std::move(landed)](Held& done) mutable {
													landed(std::move(done.found.outcome), std::move(done.found.fault));
												});
	});
}

/// The readiness item of a resource opened at step `opens`: `name` as waitingFor names it, `bound` from its asking,
/// failing start-failed with `overdue` past it.
inline ReadinessItem ioOpenItem(WaitingFor::Kind kind, std::string name, std::chrono::milliseconds bound,
                                std::string overdue) {
	return ReadinessItem{
		.kind = kind,
		.name = std::move(name),
		.step = ReadinessStep::opens,
		.bound = bound,
		.fact = Fact::configureFailed,
		.overdue = [text = std::move(overdue)] { return text; },
	};
}

} // namespace ear::detail
