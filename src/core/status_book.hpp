// A node's status and events on their way to the main loop (docs/architecture.md#status-and-events): the queue its
// writers fill from any thread, and the book the main loop keeps per node, which holds the status view and paces what
// it tells. Private to ear_core: NodeHost's.
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "ear/core/node_host.hpp"
#include "ear/core/node_status.hpp"
#include "ear/core/status_spec.hpp"
namespace ear::detail {
using StatusClock = std::chrono::steady_clock;

/// One record a configuration's writers put in its node's queue: a status write (field `index`, `value`), an event
/// (event `index`, `args` in its declared order) or a loss (`detail`).
struct ReportRecord {
	enum class Kind { status, event, lost };
	Kind kind;
	std::size_t index = 0;
	StatusValue value{};
	std::vector<StatusValue> args{};
	std::string detail{};
	StatusClock::time_point at{};
};

/// One configuration's queue (docs/architecture.md#status-and-events): any thread pushes, the main loop takes. Bounded
/// at `bound` records; a full queue drops a status write or an event and counts it, never a loss.
class ReportQueue {
public:
	static constexpr std::size_t bound = 1024;
	/// True when this record is the first since the last take: the caller posts one wake.
	bool push(ReportRecord record);
	/// Every record, in push order; the next push wakes again.
	std::deque<ReportRecord> take();
	/// The records dropped since the last call.
	std::uint64_t takeDropped();

private:
	std::mutex mutex_;
	std::deque<ReportRecord> records_; // mutex_
	std::uint64_t dropped_ = 0;        // mutex_
	bool woken_ = false;               // mutex_: a wake is posted and not yet taken
};

/// What a book tells, in order: NodeHost fans each out to its listeners (and a loss to the node).
class StatusBookEvents {
public:
	virtual void written(const StatusWrite& write) = 0;
	virtual void changed(std::span<const FieldValue> fields) = 0;
	virtual void reset(std::span<const FieldValue> fields) = 0;
	virtual void raised(const RaisedEvent& event) = 0;
	virtual void diagnosed(NodeDiagnostic diagnostic) = 0;
	virtual void lost(std::string detail) = 0;

protected:
	~StatusBookEvents() = default;
};

/// A node's status view and the paces of what it tells (docs/architecture.md#rates-and-bounds), on the main loop: per
/// field, 32 records at once then one a second, with a trailing record for changes held back; per node, 32 events at
/// once then one a second, held back ones counted in ear.events-dropped; ear.records-dropped per subject; and
/// ear.status-flapping per field.
class StatusBook {
public:
	static constexpr double burst = 32;
	static constexpr std::chrono::seconds refill{1};
	StatusBook(std::span<const StatusDeclaration* const> fields, std::span<const EventDeclaration> events);
	/// Each record in order: a status write updates the view and is offered to standard output's pace and marked for
	/// the wire; an event first sends the marked fields, then is offered to the event pace; a loss first sends the
	/// marked fields, then is told. `queueDropped` records the queue had no room for are counted.
	void apply(std::deque<ReportRecord> records, std::uint64_t queueDropped, StatusClock::time_point now,
	           StatusBookEvents& out);
	/// The tick: the marked fields sent, trailing records the pace allows written, flapping judged, drops said.
	void tick(StatusClock::time_point now, StatusBookEvents& out);
	/// When the book next needs a tick for itself (a trailing record, a flapping field to settle, a drop to say); none
	/// when it does not.
	std::optional<StatusClock::time_point> due() const;
	/// The configuration closed: the marked fields sent, then every field at its initial value, told as one reset
	/// (none for a type with no field). The paces and flapping stay the node's.
	void reset(StatusClock::time_point now, StatusBookEvents& out);
	/// The view, in declaration order.
	std::vector<FieldValue> view() const;
	bool empty() const noexcept { return fields_.empty(); }

private:
	struct Field {
		const StatusDeclaration* declared;
		StatusValue value;
		StatusValue written; // the `to` of its last record (its initial value before any)
		bool marked = false; // changed since the last statusChanged
		double tokens = burst;
		StatusClock::time_point filled{};
		std::uint64_t heldBack = 0;
		std::deque<StatusClock::time_point> transitions; // within the flapping window
		bool flapping = false;
		StatusClock::time_point lastAbove{};
		StatusClock::time_point flapSaid{};
	};
	static void refillOf(double& tokens, StatusClock::time_point& filled, StatusClock::time_point now);
	void flush(StatusBookEvents& out);
	void written(Field& field, StatusClock::time_point now, StatusBookEvents& out);
	void flapped(Field& field, StatusClock::time_point now, StatusBookEvents& out);
	void raise(const ReportRecord& record, StatusClock::time_point now, StatusBookEvents& out);
	void dropsSaid(StatusClock::time_point now, StatusBookEvents& out);
	std::vector<Field> fields_;
	std::span<const EventDeclaration> events_;
	double eventTokens_ = burst;
	StatusClock::time_point eventsFilled_{};
	std::uint64_t eventsHeld_ = 0;
	std::optional<StatusClock::time_point> eventsSaid_;
	std::uint64_t statusHeld_ = 0; // status records held back since the last ear.records-dropped of subject status
	std::optional<StatusClock::time_point> statusSaid_;
	std::uint64_t queueDropped_ = 0;
	std::optional<StatusClock::time_point> queueSaid_;
};

} // namespace ear::detail
