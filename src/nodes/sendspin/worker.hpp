// A sendspin node's two loops' states (docs/node-types.md#threads): the worker, the library's "main loop", and the
// notifier, telling the library what played. The worker owns the library's client and reads the node's key store;
// nothing else touches the client but the sink's thread, through ToServer, and the notifier, through the PlayerSlot,
// so the library never meets a realtime thread or the main loop.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "advertiser.hpp"
#include "connection_watch.hpp"
#include "ear/core/loop.hpp"
#include "ear/core/node_handle.hpp"
#include "ear/core/node_type.hpp"
#include "ear/core/store.hpp"
#include "event_fd.hpp"
#include "feed.hpp"
#include "key_store.hpp"
#include "mailbox.hpp"
#include "sendspin/client.h"
#include "sendspin/player_role.h"
#include "sendspin/source_role.h"
#include "sendspin_code.hpp"
#include "settings.hpp"
#include "to_server.hpp"
namespace ear::nodes::sendspin {
/// The lead a player reports (spec "required_lead_time_ms": at least what still plays after a stream/clear): all a feed
/// of `feedFrames` holds, and the block being played. The library reports its own pipeline's lead if that is larger.
std::uint16_t requiredLeadMs(std::uint32_t feedFrames, std::uint32_t maxBlock, SampleRate rate) noexcept;
/// The player's buffer in the library's inbound ring (PlayerRoleConfig::audio_buffer_capacity), which the worker sets:
/// the library's default, named here because the feed's undrained bound is derived from it.
inline constexpr std::size_t playerBufferBytes = ::sendspin::PlayerRoleConfig::DEFAULT_AUDIO_BUFFER_CAPACITY;
/// How long the code may leave the feed undrained before its writer stops waiting (docs/node-types.md#clock), derived
/// from what it protects: the room a server that fills the player's advertised buffer leaves in the library's inbound
/// ring, at the stream's worst-case byte rate, less the library's wait for room before it closes the connection; and
/// no less than two feeds' worth (`feedFrames` at `rate`), so a scheduling hiccup never drops.
std::chrono::nanoseconds undrainedBoundFor(SampleRate rate, std::uint32_t feedFrames) noexcept;

/// The library's player as the worker lends it to the notifier: set once the client started, cleared before the
/// client stops, so the notifier never calls a player that is going. Every call under one lock; any thread.
class PlayerSlot {
public:
	void set(::sendspin::PlayerRole* player) noexcept;
	/// notify_audio_played on the player; false (and nothing called) while there is none.
	bool notify(std::uint32_t frames, std::int64_t finishUs);

private:
	std::mutex mutex_;
	::sendspin::PlayerRole* player_ = nullptr;
};

/// What the worker's state is made with, in configure (on the main loop): references the configuration keeps until
/// teardown step 5, after the state is destroyed.
struct WorkerParts {
	NodeHandle& handle; // the configuration's: notes and losses
	Store& keys;        // the key file, open and trusted once the node is configured
	Feed& feed;         // what the library's sync thread writes
	PlayerSlot& player; // lent to the notifier
	Loop& self;         // the worker loop, its 10 ms tick
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
	Feed& feed_;
	PlayerSlot& player_;
	Loop& self_;
	SendspinSettings settings_;
	AudioFormat format_;
	std::vector<float> scratch_;           // the sync thread's; a feed's worth of floats
	std::atomic<bool> streamAsked_{false}; // the stream's format is the one the node listed
	bool streaming_ = false;               // worker only
	bool saidDialling_ = false;            // worker only
	std::unique_ptr<Timer> tick_;          // worker only
	std::optional<ConnectionWatch> watch_; // worker only, once the client started
	std::unique_ptr<Advertiser> advertiser_;
	std::unique_ptr<NodeKeyStore> keyStore_;
	::sendspin::PlayerRole* playerRole_ = nullptr; // set before the client starts
	::sendspin::SourceRole* source_ = nullptr;     // set before the client starts, and outlives every consume()
	mutable std::mutex clientMutex_;               // client_'s pointer, for clientTimeOf
	std::unique_ptr<::sendspin::SendspinClient> client_;
};

/// What the notifier tells the library, from two tellers (docs/node-types.md#clock): the code's frames (its mailbox),
/// and those the feed's writer played into nothing while no cycle drained it. One report: the frames of both since the
/// last, dated by the code's finish when the code played anything since (the graph is live), else by the feed's.
/// Dropped frames of a stream since ended are not told: the library forgot them. Pure: the notifier's state, and the
/// tests'.
class PlayedTeller {
public:
	struct Report {
		std::uint32_t frames;
		std::int64_t finishRawNs;
	};
	/// What to tell now, if anything.
	std::optional<Report> next(const std::optional<PlayProgress>& code, const Feed::Dropped& dropped) const noexcept;
	/// That report was told (the player took it): what it counted is told.
	void told(const std::optional<PlayProgress>& code, const Feed::Dropped& dropped) noexcept;

private:
	std::uint64_t reported_ = 0;        // of the code's frames
	std::uint64_t reportedDropped_ = 0; // of the feed's dropped frames
};

/// The notifier loop's state: tells the library what played, one call per ring of the code's bell. Apart from the
/// worker, whose writes sync to disk: a late notify can cost a hard sync. Made in configure; begin() watches the bell
/// once the node is configured; destroyed on its loop at teardown step 4, the watch with it.
class Notifier {
public:
	/// `bell` and `progress` (the code's), `feed` (what its writer dropped) and `player` outlive the state.
	Notifier(Loop& self, const EventFd& bell, const Mailbox<PlayProgress>& progress, const Feed& feed,
	         PlayerSlot& player) noexcept;
	/// The notifier loop, once.
	void begin();

private:
	void notify();
	Loop& self_;
	const EventFd& bell_;
	const Mailbox<PlayProgress>& progress_;
	const Feed& feed_;
	PlayerSlot& player_;
	PlayedTeller teller_;
	std::unique_ptr<Timer> watch_;
};
} // namespace ear::nodes::sendspin
