// A configuration's notes, at a rate no peer can raise (docs/writing-a-node-type.md#the-node-handle): each note goes on
// the main loop's queue and into the log, and a peer can drive a network node's notes as fast as it runs pairing
// attempts. A burst passes, as a whole normal session does; then one a second, and what was held back is counted and
// said once a note can pass. Judged on the thread that says, before anything is queued. Private to ear_core: each
// configuration's Channel keeps one, so the rate is per node and kept across a stop and a start (they keep the
// configuration) and fresh after a restart (a new configuration).
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ear::detail {

/// What the framework says, as the configuration's own note, before the next note that passes after some were held.
std::string notesDropped(std::uint64_t count); // "notes-dropped <count>"

/// A token bucket of 32 notes, refilled at one a second. Not synchronised: its owner serialises the calls.
class NoteGate {
public:
	using Clock = std::function<std::chrono::steady_clock::time_point()>;
	static constexpr double burst = 32.0;
	static constexpr double perSecond = 1.0;
	/// `now`: the clock the rate counts on (Timers::now, or a test's own); read on the calling thread.
	explicit NoteGate(Clock now);
	/// Whether a note said now passes: if it does, how many were held back since the last that passed (0 for none),
	/// and that count starts again; if not, it is counted.
	std::optional<std::uint64_t> pass();

private:
	Clock now_;
	double tokens_;
	std::chrono::steady_clock::time_point refilled_;
	std::uint64_t dropped_ = 0;
};

} // namespace ear::detail
