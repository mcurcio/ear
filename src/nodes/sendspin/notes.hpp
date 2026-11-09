// The texts a sendspin node says (docs/node-types.md#notes): each one's one home. status_log.cpp frames them
// as `node <n> says <text>`.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include "peer_text.hpp"
namespace ear::sendspin::notes {
/// What a peer sent, safe on one status line: every control character (peer_text.hpp) and every byte that is no UTF-8
/// becomes '?', and at most 64 bytes of it are read, cut at a character's start.
inline std::string printable(std::string_view text) {
	constexpr std::size_t most = 64;
	if (text.size() > most) {
		std::size_t cut = most;
		while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) --cut;
		text = text.substr(0, cut);
	}
	std::string safe;
	safe.reserve(text.size());
	for (std::size_t at = 0; at < text.size();) {
		const auto character = characterAt(text, at);
		if (character && !isControl(character->code)) safe += text.substr(at, character->bytes);
		else safe += '?';
		at += character ? character->bytes : 1;
	}
	return safe;
}
inline std::string id(std::string_view clientId) { return std::format("id {}", clientId); }
inline std::string dialling(std::string_view url) { return std::format("dialling {}", url); }
inline std::string listening(std::uint16_t port, std::string_view instance) {
	return std::format("listening {} as {}", port, instance);
}
/// Quoted, so a name cannot pass for the word after it: a quote inside it becomes '?' too.
inline std::string connected(std::string_view serverName, bool paired) {
	std::string name = printable(serverName);
	std::ranges::replace(name, '"', '?');
	return std::format("connected \"{}\" {}", name, paired ? "paired" : "unpaired");
}
inline std::string disconnected() { return "disconnected"; }
inline std::string stream(std::string_view codec, std::uint32_t rate, std::uint32_t channels, std::uint32_t bits) {
	return std::format("stream {} {} {} {}", codec, rate, channels, bits);
}
inline std::string streamEnded() { return "stream-ended"; }
/// Six digits, grouped as an operator reads them.
inline std::string pairingCode(std::string_view digits) {
	return std::format("pairing-code {}-{}", digits.substr(0, digits.size() / 2), digits.substr(digits.size() / 2));
}
inline std::string pairingCodeCleared() { return "pairing-code-cleared"; }
inline std::string paired(std::string_view serverId) { return std::format("paired {}", printable(serverId)); }
inline std::string pairingFailed(std::string_view reason) { return std::format("pairing-failed {}", reason); }
inline std::string pairingHeld() { return "pairing-held"; }
inline std::string sending(std::uint32_t rate, std::uint32_t channels, std::uint32_t bits) {
	return std::format("sending {} {} {}", rate, channels, bits);
}
inline std::string sendingStopped() { return "sending-stopped"; }
inline std::string keyWriteFailed(std::string_view detail) { return std::format("key-write-failed {}", detail); }
inline std::string advertiseFailed(std::string_view detail) { return std::format("advertise-failed {}", detail); }
inline std::string notesDropped(std::uint64_t count) { return std::format("notes-dropped {}", count); }
} // namespace ear::sendspin::notes
