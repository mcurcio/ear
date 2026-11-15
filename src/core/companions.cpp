// Companions' kinds, their readiness deadlines (ear/core/companions.hpp; docs/engine.md#readiness-deadlines) and the
// module-run converters' latency rule (docs/engine.md#module-run-converters), and what each engine companion claims
// (docs/writing-a-node-type.md#claims).
#include "ear/core/companions.hpp"

namespace ear {

Delay moduleRunDelay(const RtpReceiverShape& shape, FrameClock graph) noexcept {
	const auto buffered = static_cast<std::uint32_t>(graph.frames(std::chrono::milliseconds{shape.latencyMs}));
	return Delay{Latency{buffered, buffered}};
}

Delay moduleRunDelay(const PipeReceiverShape& shape, FrameClock graph) noexcept {
	// The module's frames at the writer's rate, as time, then in graph frames to the nearest, half up: frames and time
	// convert only through FrameClock (docs/engine.md#dating). The time is truncated to the nanosecond, far below the
	// half a graph frame that could tip the rounding.
	const std::chrono::nanoseconds held{FrameClock(shape.rate).ns(pipeReceiverBufferFrames)};
	const auto buffered = static_cast<std::uint32_t>(graph.framesNearest(held));
	return Delay{Latency{buffered, buffered}};
}

std::vector<Claim> claimsOf(const RtpReceiverShape& shape) {
	return {Claim{Claim::Kind::udp, std::to_string(shape.port), shape.address.multicast() ? shape.address.text() : ""}};
}

std::vector<Claim> claimsOf(const SoundCardShape&) { return {}; }

std::vector<Claim> claimsOf(const PipeReceiverShape&) { return {}; }

std::string_view companionKindName(CompanionKind kind) noexcept {
	switch (kind) {
	case CompanionKind::cardPlayback:
		return "card-playback";
	case CompanionKind::cardCapture:
		return "card-capture";
	case CompanionKind::rtpReceiver:
		return "rtp-receiver";
	case CompanionKind::pipeReceiver:
		return "pipe-receiver";
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
	case CompanionKind::pipeReceiver:
		return pipeReceiverReadinessBound;
	case CompanionKind::sink:
		return sinkReadinessBound;
	case CompanionKind::source:
		return std::nullopt;
	}
	return std::nullopt;
}

} // namespace ear
