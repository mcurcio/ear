// Companions' kinds and their readiness deadlines (ear/core/companions.hpp; docs/engine.md#readiness-deadlines).
#include "ear/core/companions.hpp"

namespace ear {

std::string_view companionKindName(CompanionKind kind) noexcept {
	switch (kind) {
	case CompanionKind::cardPlayback:
		return "card-playback";
	case CompanionKind::cardCapture:
		return "card-capture";
	case CompanionKind::rtpReceiver:
		return "rtp-receiver";
	case CompanionKind::sink:
		return "sink";
	case CompanionKind::source:
		return "source";
	}
	return "source";
}

std::optional<std::chrono::milliseconds> readinessBound(CompanionKind kind) noexcept {
	switch (kind) {
	case CompanionKind::cardPlayback:
	case CompanionKind::cardCapture:
		return cardReadinessBound;
	case CompanionKind::rtpReceiver:
		return rtpReceiverReadinessBound;
	case CompanionKind::sink:
		return sinkReadinessBound;
	case CompanionKind::source:
		return std::nullopt;
	}
	return std::nullopt;
}

} // namespace ear
