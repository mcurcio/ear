// Gestures (docs/architecture.md#gestures): drags of live ramped decimals from the control port, the main loop their
// one owner. A gesture moves the live values of every pair of its reach at each accepted tick, and ends by a commit
// of ear.conf (the keeper's), an abort (the slots back to the committed settings) or a release ear starts for an owner
// that closed or fell silent. Main thread only.
#pragma once
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "ear/core/node_host.hpp"
#include "ear/core/timers.hpp"
namespace ear {
/// Which control-port session a gesture belongs to: an opaque number the control port gives each session.
using SessionToken = std::uint64_t;
/// One node's key.
struct GestureKey {
	NodeName node;
	std::string key;
	friend bool operator==(const GestureKey&, const GestureKey&) = default;
};
/// One pair of a gesture's reach and its slot among its node's live members (NodeType::liveIndexOf).
struct GesturePair {
	GestureKey at;
	std::uint32_t member;
};
/// What a begin opens, judged by its caller (the reach and its range from the graph file, the holds by heldAt).
struct GestureOpen {
	SessionToken session;
	std::vector<GesturePair> reach; // the pair asked for first
	double low;
	double high;
	unsigned decimals;
	double committed; // the value ear.conf gives the reach now
};
enum class GesturePhase { begin, change, end, aborted, tickRefused };
/// What a gesture's listeners are told (docs/architecture.md#gestures).
struct GestureNote {
	std::string gesture;
	GesturePhase phase;
	SessionToken owner;
	/// Another session that ended or cancelled it by its id, told its end or abort too.
	std::optional<SessionToken> by{};
	/// begin: the committed value; change: the newest accepted; end: what was committed; aborted: what the slots went
	/// back to, absent after a related change of the file (the change's own events say it).
	std::optional<double> value{};
	std::vector<GestureKey> reach{}; // every phase but tick-refused
	/// end: `session-closed` or `stale` when ear ended it for its owner, else empty; aborted: why.
	std::string reason{};
	std::string code{}; // tick-refused: invalid-settings
	std::string key{};  // tick-refused: the key refused
	std::string detail{};
	std::optional<double> handValue{}; // aborted: the last value the hand left in the slots
};
class GestureListener {
public:
	virtual void gestureChanged(const GestureNote& note) = 0;

protected:
	~GestureListener() = default;
};
/// An end for the keeper to commit: the value, rounded to the reach's decimals, at the first pair's definition.
struct GestureEnding {
	std::string gesture;
	GestureKey at;
	double value;
	std::vector<GestureKey> reach;
	std::string reason; // a release's: session-closed or stale; empty for a gesture.end
	SessionToken by;    // the session whose end it is (the owner's, for a release)
};
enum class GestureRefusal { unknownGesture };
/// One open gesture as the related-change rule reads it.
struct OpenGesture {
	std::string gesture;
	std::vector<GestureKey> reach;
	bool ending;
};
/// The silence after which an owner's gesture is released `stale` (docs/architecture.md#gestures).
inline constexpr std::chrono::milliseconds gestureStaleAfter{30'000};
/// The most often a gesture's moves are told: one per status tick's pace.
inline constexpr std::chrono::milliseconds gestureChangePace{33};
/// The most often a gesture's owner is told its ticks were refused.
inline constexpr std::chrono::milliseconds gestureRefusalPace{1'000};
class Gestures {
public:
	/// All outlive it. `release` takes each end ear starts for an owner (the keeper commits it like gesture.end).
	Gestures(LiveTarget& nodes, Timers& timers, std::chrono::milliseconds staleAfter = gestureStaleAfter);
	~Gestures();
	Gestures(const Gestures&) = delete;
	Gestures& operator=(const Gestures&) = delete;
	void subscribe(GestureListener& listener); // before the first open
	/// Who commits a release (before the first open): the keeper, through the gateway.
	void releaseThrough(std::move_only_function<void(GestureEnding)> release);
	/// The open gesture moving `at`, and how long its owner has been silent; none when no gesture holds it.
	struct Held {
		std::string gesture;
		std::chrono::milliseconds silent;
	};
	std::optional<Held> heldAt(const GestureKey& at) const;
	/// Opens a gesture: a new id of 128 random bits in hex, its silence timer armed; tells begin. Writes nothing.
	std::string open(GestureOpen open);
	/// A tick from `session`: dropped unless the gesture is open, not ending and the session's own, or when `seq` is
	/// not above the last taken; else its silence timer re-armed, and a value equal to the last accepted writes nothing.
	/// Otherwise each reached node's check with the value in place (LiveTarget::checkLive, every other gesture's hand
	/// value already in its live values): a refusal is tick-refused to the owner, at most once a second, and the last
	/// accepted value stays; else every reached node's live values and slots take it and a change is told, at most
	/// once per gestureChangePace (the newest).
	void move(const std::string& gesture, SessionToken session, std::uint64_t seq, double value);
	/// A gesture.end read: the gesture marked ending (no tick taken again, its timer stopped) and its value (`value`,
	/// else the last accepted) rounded to the reach's decimals. unknownGesture when no open gesture has the id or its
	/// end was already read.
	std::expected<GestureEnding, GestureRefusal> end(const std::string& gesture, std::optional<double> value,
	                                                 SessionToken by);
	/// A gesture's end committed (or equal to the committed value): every reached slot set to its node's committed
	/// value; tells end with `value`. Nothing for an id that is not open.
	void close(const std::string& gesture, double value);
	/// Aborts a gesture: tells aborted with `reason` and the value the slots go back to (none when `toldByChange`: a
	/// related change of the file says it), and every reached slot is set to its node's committed value at the next
	/// settle(); with `settleNow`, at once. Nothing for an id that is not open.
	void abort(const std::string& gesture, const std::string& reason, bool toldByChange = false, bool settleNow = true);
	/// Every slot an abort left unsettled set to its node's committed value: last in a turn that committed a change.
	void settle();
	/// gesture.cancel from `by`: aborted, reason cancelled. unknownGesture when no open gesture has the id.
	std::expected<void, GestureRefusal> cancel(const std::string& gesture, SessionToken by);
	/// The session closed: each of its gestures not ending is released (session-closed).
	void sessionClosed(SessionToken session);
	/// Stopping: every gesture forgotten, nothing committed, nothing told (docs/architecture.md#gestures).
	void drop();
	std::vector<OpenGesture> openGestures() const;
	/// Each open gesture moving a key of `node`, with its key and the value its hand holds: the graph's per-node
	/// `gestures` (docs/architecture.md#gestures).
	struct OnNode {
		std::string gesture;
		std::string key;
		double value;
	};
	std::vector<OnNode> onNode(const NodeName& node) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace ear
