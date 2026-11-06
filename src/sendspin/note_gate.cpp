#include "note_gate.hpp"

#include <algorithm>
#include <utility>

#include "notes.hpp"

namespace ear::sendspin {
namespace {

constexpr double burstNotes = 32.0;
constexpr double notesPerSecond = 1.0;

} // namespace

NoteGate::NoteGate(InsidesEvents& next, std::function<Clock::time_point()> now)
    : next_(next), now_(std::move(now)), tokens_(burstNotes), refilled_(now_()) {}

void NoteGate::say(std::string text) {
	std::uint64_t saidDropped = 0;
	{
		const std::scoped_lock lock(mutex_);
		const Clock::time_point at = now_();
		tokens_ = std::min(burstNotes, tokens_ + std::chrono::duration<double>(at - refilled_).count() * notesPerSecond);
		refilled_ = at;
		if (tokens_ < 1.0) {
			++dropped_;
			return;
		}
		tokens_ -= 1.0;
		saidDropped = std::exchange(dropped_, 0);
	}
	if (saidDropped > 0) next_.say(notes::notesDropped(saidDropped));
	next_.say(std::move(text));
}

void NoteGate::lost(std::string detail) { next_.lost(std::move(detail)); }

} // namespace ear::sendspin
