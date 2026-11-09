// The connection watch (docs/node-types.md#failures): the pure judge of the connection. No connection
// event exists upstream, so the worker feeds it each reading of the library's getters, is_paired() first (the guide:
// a true is_connected() read after it means that session is activated).
#pragma once
#include <chrono>
#include <optional>
namespace ear::sendspin {
class ConnectionWatch {
public:
	struct Reading {
		bool paired;
		bool connected;
	};
	struct Told {
		bool dial;
		bool connected;
		bool disconnected;
	}; // `connected` again when `paired` changed
	explicit ConnectionWatch(bool dials) noexcept : dials_(dials) {}
	/// Never `dial` within 31 s of the last dial. `disconnected` only after `connected` and 5 s of false.
	Told read(Reading now, std::chrono::steady_clock::time_point at) noexcept;

private:
	bool dials_;
	bool toldConnected_ = false;
	bool toldPaired_ = false;
	std::optional<std::chrono::steady_clock::time_point> falseSince_;
	std::optional<std::chrono::steady_clock::time_point> lastDial_;
};
} // namespace ear::sendspin
