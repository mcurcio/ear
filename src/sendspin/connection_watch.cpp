#include "connection_watch.hpp"

namespace ear::sendspin {
namespace {

/// is_connected() reads false through a re-handshake, so a shorter false says nothing.
constexpr std::chrono::seconds disconnectedAfter{5};
/// Above the library's 30 s reap of a stalled attempt, because a new dial releases a live one with another_server.
constexpr std::chrono::seconds redialAfter{31};

} // namespace

ConnectionWatch::Told ConnectionWatch::read(Reading now, std::chrono::steady_clock::time_point at) noexcept {
	Told told{};
	if (now.connected) {
		falseSince_.reset();
		told.connected = !toldConnected_ || now.paired != toldPaired_;
		toldConnected_ = true;
		toldPaired_ = now.paired;
		return told;
	}
	if (toldConnected_) {
		if (!falseSince_) falseSince_ = at;
		if (at - *falseSince_ < disconnectedAfter) return told;
		told.disconnected = true;
		toldConnected_ = false;
		falseSince_.reset();
	}
	if (dials_ && (!lastDial_ || at - *lastDial_ >= redialAfter)) {
		told.dial = true;
		lastDial_ = at;
	}
	return told;
}

} // namespace ear::sendspin
