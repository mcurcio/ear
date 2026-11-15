// A sendspin node's two loops' states (docs/node-types.md#threads): the worker, the library's "main loop", and the
// notifier, telling the library what played. The worker owns the library's client and reads the node's key store;
// nothing else touches the client but the sink's thread, through ToServer, and the notifier, through the PlayerSlot,
// so the library never meets a realtime thread or the main loop.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "connection_watch.hpp"
#include "ear/core/loop.hpp"
#include "ear/core/node_handle.hpp"
#include "ear/core/node_type.hpp"
#include "ear/core/source.hpp"
#include "ear/core/store.hpp"
#include "ear/mdns/advertiser.hpp"
#include "key_store.hpp"
#include "sendspin/client.h"
#include "sendspin/player_role.h"
#include "sendspin/source_role.h"
#include "sendspin_clock.hpp"
#include "settings.hpp"
#include "to_server.hpp"
namespace ear::nodes::sendspin {
/// The lead a player reports (spec "required_lead_time_ms": at least what still plays after a stream/clear): all a
/// source ring of `sourceFrames` holds, and the block being played. The library reports its own pipeline's lead if
/// that is larger.
std::uint16_t requiredLeadMs(std::uint32_t sourceFrames, std::uint32_t maxBlock, SampleRate rate) noexcept;
/// The player's buffer in the library's inbound ring (PlayerRoleConfig::audio_buffer_capacity), which the worker sets:
/// the library's default, named here because the clock's undrained bound is derived from it.
inline constexpr std::size_t playerBufferBytes = ::sendspin::PlayerRoleConfig::DEFAULT_AUDIO_BUFFER_CAPACITY;
/// The player's one format: the graph frequency (spec "player@v1", the server resamples) and one depth, so nothing the
/// sync thread writes can change depth under it: on_audio_write carries no format, and a stream/start reaches the
/// worker while chunks of the last format are still being written.
inline constexpr std::uint32_t playerBytesPerSample = 3;
inline constexpr std::uint32_t playerBytesPerFrame = playerBytesPerSample * nodeChannels;
/// How long no tick may drain the source before the clock's writer stops waiting on it (docs/node-types.md#clock),
/// derived from what it protects: the room a server that fills the player's advertised buffer leaves in the library's
/// inbound ring, at the stream's worst-case byte rate, less the library's wait for room before it closes the
/// connection; and no less than two rings' worth (`sourceFrames` at `rate`), so a scheduling hiccup never paces.
/// Constant, so a test can hold its stalls against it at compile time.
constexpr std::chrono::nanoseconds undrainedBoundFor(SampleRate rate, std::uint32_t sourceFrames) noexcept {
	// The library advertises (N-1)/N of the player's quota to the server as buffer_capacity, N being 3
	// (player_role.cpp, AUDIO_BUFFER_ADVERTISE_DENOMINATOR at the pinned commit), so a server that fills it leaves a
	// third of the quota for what arrives while nothing is taken. A ring that cannot take a message within 100 ms
	// (inbound_ring.h, INBOUND_ACQUIRE_TIMEOUT_MS) closes the connection. FLAC's worst case is its verbatim frames, as
	// large as the PCM they carry: 24-bit stereo at the graph's rate, the one format the player lists.
	constexpr std::int64_t advertiseDenominator = 3;
	constexpr std::chrono::milliseconds acquireTimeout{100};
	constexpr std::int64_t room = static_cast<std::int64_t>(playerBufferBytes) / advertiseDenominator;
	const std::int64_t bytesPerSecond = std::int64_t{rate.hz()} * playerBytesPerFrame;
	const std::chrono::nanoseconds lasts{room * 1'000'000'000 / bytesPerSecond};
	const std::chrono::nanoseconds floor{FrameClock(rate).ns(std::int64_t{sourceFrames} * 2)};
	return std::max<std::chrono::nanoseconds>(lasts - acquireTimeout, floor);
}

/// The library's player as the worker lends it to the notifier: set once the client started, cleared before the
/// client stops, so the notifier never calls a player that is going. Every call under one lock; any thread. The same
/// lock orders a report against the stream's end: a report is judged against the clock's current stream and told under
/// it, and the end clears the clock under it, so no report of an ended stream reaches the library after its end.
class PlayerSlot {
public:
	void set(::sendspin::PlayerRole* player) noexcept;
	enum class Told { nothing, told, noPlayer };
	/// notify_audio_played on the player with what `progress` holds past `reported` of `clock`'s current stream
	/// (framesToTell), dated by its finish: `nothing` when there is nothing to tell, `noPlayer` (nothing called) while
	/// there is no player.
	Told notify(const SendspinClock& clock, const PlayProgress& progress, std::uint64_t reported);
	/// The worker's on_stream_end: clears `clock` (SendspinClock::clear) under the lock.
	void endStream(SendspinClock& clock);

private:
	std::mutex mutex_;
	::sendspin::PlayerRole* player_ = nullptr;
};

/// What the worker's state is made with, in configure (on the main loop): references the configuration keeps until
/// teardown step 5, after the state is destroyed.
struct WorkerParts {
	NodeHandle& handle;   // the configuration's: notes and losses
	Store& keys;          // the key file, open and trusted once the node is configured
	SendspinClock& clock; // what the library's sync thread writes through, and the stream's end told to
	SourceWriter& writer; // the source's: its size, and the writer's idle
	PlayerSlot& player;   // lent to the notifier
	Loop& self;           // the worker loop, its 10 ms tick
	SendspinSettings settings;
	AudioFormat format;
};

/// The worker loop's state. Made in configure with no I/O and nothing armed; begin() runs once the node is configured
/// and makes the client on the tick's first run; the client lives as long as the configuration, so a stop and a start
/// keep its connection and pairing (docs/node-types.md#stop-restart-disable). Destroyed on its loop at teardown step 4:
/// the mDNS goodbye, the client stopped (the Sendspin goodbye, its threads joined) and destroyed.
class Worker final : public ::sendspin::PlayerRoleListener,
					 public ::sendspin::SendspinNetworkProvider,
					 public ::sendspin::SendspinClientListener,
					 public ::sendspin::SourceRoleListener,
					 public SourceSink {
public:
	explicit Worker(WorkerParts parts);
	~Worker() override;
	Worker(const Worker&) = delete;
	Worker& operator=(const Worker&) = delete;
	/// The worker loop, once: arms the tick, whose first run reads the key record and makes, starts and (listening)
	/// advertises the client, and whose every later run turns the library, reads the connection and answers mDNS.
	void begin();
	/// The sink on `to_server`'s consumer (consumedBy(worker, &Worker::toServer)): its calls are the sink's thread's.
	ToServer toServer;
	/// Any thread: the library's time filter, for the tests; 0 before the client is made.
	std::int64_t clientTimeOf(std::int64_t serverUs) const;

	// SendspinNetworkProvider. Nothing to wait for: the library looks a host up when it dials, and a listener binds now.
	bool is_network_ready() override { return true; }
	// PlayerRoleListener: on_audio_write on the library's sync thread, the rest on the worker.
	std::size_t on_audio_write(std::uint8_t* data, std::size_t length, std::uint32_t timeout_ms) override;
	void on_stream_start() override;
	void on_stream_end() override;
	// SendspinClientListener. The worker. Pairing is the connection's domain: notes, never node states.
	void on_pairing_succeeded(const std::string& server_id) override;
	void on_pairing_failed(const std::string& server_id, ::sendspin::SendspinPairAbortReason reason) override;
	void on_display_pairing_code(const std::string& code, ::sendspin::SendspinPairingCodeFormat format) override;
	void on_clear_pairing_code() override;
	void on_open_pairing_window() override;
	// SourceRoleListener. The worker.
	void on_streaming_started() override;
	void on_streaming_stopped() override;
	// SourceSink. The sink's thread.
	void writeAudio(const std::uint8_t* pcm, std::size_t bytes, std::int64_t stampUs) override;

private:
	void tick();
	void startClient();
	std::unique_ptr<::sendspin::SendspinClient> makeClient(NodeKeyStore& keys);
	void startListening(std::string_view clientId);
	void turn();
	/// A loss: the tick stops, and the framework tears the node down (its state destroyed, the client with it).
	void fail(std::string detail);

	NodeHandle& handle_;
	Store& keys_;
	SendspinClock& clock_;
	SourceWriter& writer_;
	PlayerSlot& player_;
	Loop& self_;
	SendspinSettings settings_;
	AudioFormat format_;
	std::vector<float> scratch_;           // the sync thread's; a ring's worth of floats
	std::atomic<bool> streamAsked_{false}; // the stream's format is the one the node listed
	bool streaming_ = false;               // worker only
	bool saidDialling_ = false;            // worker only
	std::unique_ptr<Timer> tick_;          // worker only
	std::optional<ConnectionWatch> watch_; // worker only, once the client started
	std::unique_ptr<ear::mdns::Advertiser> advertiser_;
	std::unique_ptr<NodeKeyStore> keyStore_;
	::sendspin::PlayerRole* playerRole_ = nullptr; // set before the client starts
	::sendspin::SourceRole* source_ = nullptr;     // set before the client starts, and outlives every consume()
	mutable std::mutex clientMutex_;               // client_'s pointer, for clientTimeOf
	std::unique_ptr<::sendspin::SendspinClient> client_;
};

/// The notifier loop's state: tells the library what played, one call per ring of the clock's wake. Apart from the
/// worker, whose writes sync to disk: a late notify can cost a hard sync. Made in configure; begin() watches the wake
/// once the node is configured; destroyed on its loop at teardown step 4, the watch with it.
class Notifier {
public:
	/// `clock` (the source's) and `player` outlive the state.
	Notifier(Loop& self, SendspinClock& clock, PlayerSlot& player) noexcept;
	/// The notifier loop, once.
	void begin();
	/// Test seam, any thread: the reports told whose finish was earlier than the last told. Only what lies downstream
	/// shrinking may make one (docs/node-types.md#clock): the clock's one timeline never steps back otherwise.
	std::uint64_t steppedBack() const noexcept { return steppedBack_.load(std::memory_order_acquire); }

private:
	void notify();
	Loop& self_;
	SendspinClock& clock_;
	PlayerSlot& player_;
	std::uint64_t reported_ = 0; // the clock's count as last told
	std::optional<std::int64_t> lastFinish_;
	std::atomic<std::uint64_t> steppedBack_{0};
	std::unique_ptr<Timer> watch_;
};
} // namespace ear::nodes::sendspin
