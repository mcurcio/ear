// What a configuration still waits for before it is configured (docs/engine.md#readiness-deadlines;
// docs/architecture.md#the-node-lifecycle): every kind NodeHost waits on is an item of one list, asked in steps, each
// bounded by one timer mechanism, read for waitingFor and judged when its bound passes. Private to ear_core: NodeHost
// keeps one per configuration.
#pragma once
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ear/core/node_host.hpp"
#include "ear/core/node_lifecycle.hpp"
#include "ear/core/timers.hpp"

namespace ear::detail {

/// When an item is asked: only once every item of every earlier step is ready, so a later item may rely on what an
/// earlier one made. The order is the lifecycle's (docs/architecture.md#the-node-lifecycle); today only `companions`
/// has items (engine companions and sinks' consumers).
enum class ReadinessStep : unsigned { claims, opens, loops, companions };

/// The index of an item not added yet: never ready.
inline constexpr std::size_t noReadiness = static_cast<std::size_t>(-1);

/// One thing a configuration waits for. Main thread.
struct ReadinessItem {
	WaitingFor::Kind kind; // the wire's waitingFor kind
	std::string name;      // its subject as waitingFor names it: empty where the wire names none (a consumer)
	ReadinessStep step;
	std::optional<std::chrono::milliseconds> bound; // from when it is asked; none: bounded elsewhere
	Fact fact;                                      // what its bound passing fails the node with
	std::move_only_function<std::string()> overdue; // the failure detail, read when its bound passes
};

/// An item whose bound passed before it was ready: the fact and detail its node fails with.
struct ReadinessOverdue {
	Fact fact;
	std::string detail;
};

/// One configuration's items, in the order they were added. Main thread. Items are added, then begin() asks the first
/// step; each item is ready once (ready()), or its bound passes (passed()). A bound that passes unready ends the list:
/// nothing more is asked and no other bound is judged.
class ReadinessList {
public:
	/// Arms item `index`'s bound: a timer whose firing calls passed(index), unless destroyed first (Timers::after).
	/// The owner's, so every kind's bound shares one rule for a timer that fires late.
	using Arm = std::move_only_function<std::unique_ptr<Timer>(std::size_t index, std::chrono::milliseconds bound)>;

	/// Before begin(). Its index, which ready() and passed() take.
	std::size_t add(ReadinessItem item);
	/// Once: asks every item of the earliest step that has one not ready (and the next, while a step is all ready).
	void begin(Arm arm);
	/// Item `index` is ready: its bound cancelled, and once its step is all ready the next step asked. Again is nothing.
	void ready(std::size_t index);
	/// Item `index`'s bound passed: its failure, unless it became ready first or the list already ended.
	std::optional<ReadinessOverdue> passed(std::size_t index);
	/// Every bound cancelled (the configuration is going); what is ready stays read.
	void disarm() noexcept;

	bool isReady(std::size_t index) const noexcept;
	/// Every item is ready (and so with none).
	bool done() const noexcept;
	/// What it still waits for, in item order: each item not ready, as its kind and name, a repeated one once.
	std::vector<WaitingFor> waiting() const;

private:
	struct Entry {
		ReadinessItem item;
		bool asked = false;
		bool ready = false;
		std::unique_ptr<Timer> bound; // while asked and not ready
	};
	/// Asks every item of the earliest step not all ready, if that step is not asked yet.
	void askNext();

	std::vector<Entry> entries_;
	Arm arm_;
	bool begun_ = false;
	bool ended_ = false; // a bound passed unready
};

} // namespace ear::detail
