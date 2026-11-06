// The texts a sendspin node says (docs/design/sendspin.md section 9): each one's one home. status_log.cpp frames them
// as `node <n> says <text>`.
#pragma once
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
namespace ear::sendspin::notes {
/// What a peer sent, safe on one status line: control bytes become '?', and at most 64 bytes, cut at a character's
/// start.
inline std::string printable(std::string_view text) {
  constexpr std::size_t most = 64;
  if (text.size() > most) {
    std::size_t cut = most;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) --cut;
    text = text.substr(0, cut);
  }
  std::string safe(text);
  for (char& c : safe) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = '?';
  }
  return safe;
}
inline std::string dialling(std::string_view url) { return std::format("dialling {}", url); }
inline std::string connected(std::string_view serverName, bool paired) {
  return std::format("connected {} {}", printable(serverName), paired ? "paired" : "unpaired");
}
inline std::string disconnected() { return "disconnected"; }
inline std::string stream(std::string_view codec, std::uint32_t rate, std::uint32_t channels, std::uint32_t bits) {
  return std::format("stream {} {} {} {}", codec, rate, channels, bits);
}
inline std::string streamEnded() { return "stream-ended"; }
}
