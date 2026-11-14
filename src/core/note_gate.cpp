#include "note_gate.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace ear::detail {

std::string notesDropped(std::uint64_t count) { return std::format("notes-dropped {}", count); }

NoteGate::NoteGate(Clock now) : now_(std::move(now)), tokens_(burst), refilled_(now_()) {}

std::optional<std::uint64_t> NoteGate::pass() {
	const auto at = now_();
	tokens_ = std::min(burst, tokens_ + std::chrono::duration<double>(at - refilled_).count() * perSecond);
	refilled_ = at;
	if (tokens_ < 1.0) {
		++dropped_;
		return std::nullopt;
	}
	tokens_ -= 1.0;
	return std::exchange(dropped_, 0);
}

} // namespace ear::detail
