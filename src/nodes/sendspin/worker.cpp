// The library's client behind two threads (docs/node-types.md#threads). The worker is what the library calls its main
// loop: start, loop, stop and destruction, every listener callback but on_audio_write, the connection watch, and a
// listening node's mDNS. It waits in poll() on its stop signal, 10 ms at most: the library has no wake hook for its
// inbox and no event for a lost connection or a failed dial, so this timer stays inside this adapter.
#include "worker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <poll.h>
#include <pthread.h>

#include "advertiser.hpp"
#include "connection_watch.hpp"
#include "key_store.hpp"
#include "note_gate.hpp"
#include "notes.hpp"
#include "pcm.hpp"
#include "sendspin/client.h"
#include "sendspin/player_role.h"
#include "sendspin/source_role.h"
#include "to_server.hpp"

namespace ear::sendspin {
namespace {

constexpr int loopPollMs = 10;
/// The player's one format: the graph frequency (spec "player@v1", the server resamples) and one depth, so nothing the
/// sync thread writes can change depth under it: on_audio_write carries no format, and a stream/start reaches the
/// worker while chunks of the last format are still being written.
constexpr std::uint32_t playerBytesPerSample = 3;
constexpr std::uint32_t playerBytesPerFrame = playerBytesPerSample * nodeChannels;

void nameThread(std::string_view prefix, std::string_view node) {
	std::string name(prefix);
	name += node;
	name.resize(std::min<std::size_t>(name.size(), 15)); // the kernel's limit, without its terminator
	::pthread_setname_np(::pthread_self(), name.c_str());
}

std::string_view codecName(::sendspin::SendspinCodecFormat codec) noexcept {
	switch (codec) {
	case ::sendspin::SendspinCodecFormat::FLAC:
		return "flac";
	case ::sendspin::SendspinCodecFormat::PCM:
		return "pcm";
	default:
		return "opus";
	}
}

std::string_view abortReasonName(::sendspin::SendspinPairAbortReason reason) noexcept {
	using Reason = ::sendspin::SendspinPairAbortReason;
	switch (reason) {
	case Reason::ATTEMPT_TIMEOUT:
		return "attempt-timeout";
	case Reason::CONCURRENT_ATTEMPT:
		return "concurrent-attempt";
	case Reason::METHOD_NOT_SUPPORTED:
		return "method-not-supported";
	case Reason::PAIRING_CODE_MISMATCH:
		return "pairing-code-mismatch";
	case Reason::USER_CANCELLED:
		return "user-cancelled";
	default:
		return "unknown";
	}
}

} // namespace

class SendspinWorker::Impl final : public ::sendspin::PlayerRoleListener,
								   public ::sendspin::SendspinNetworkProvider,
								   public ::sendspin::SendspinClientListener,
								   public ::sendspin::SourceRoleListener,
								   public SourceSink {
public:
	Impl(std::shared_ptr<const folder::ConfinedFolder> folder, NodeName node, SendspinSettings settings,
	     AudioFormat format, Feed& feed, InsidesEvents& events)
		: folder_(std::move(folder)), node_(std::move(node)), settings_(std::move(settings)), format_(format),
		  feed_(feed), events_(events), scratch_(std::size_t{feed.capacityFrames()} * nodeChannels),
		  toServer_(format.rate, *this, capture_) {
		worker_ = std::thread([this] { run(); });
	}

	~Impl() override {
		// Without stop() (a start that went wrong after this was made) the worker is asked here.
		stop_.ring();
		if (worker_.joinable()) worker_.join();
	}

	void stop(std::move_only_function<void()> stopped) {
		{
			const std::scoped_lock lock(clientMutex_);
			stopped_ = std::move(stopped);
		}
		stop_.ring();
	}

	const EventFd& bell() const noexcept { return bell_; }
	Mailbox<PlayProgress>& progress() noexcept { return progress_; }
	HandoffConsumer& toServer() noexcept { return toServer_; }
	Mailbox<CaptureAnchor>& capture() noexcept { return capture_; }

	std::int64_t clientTimeOf(std::int64_t serverUs) const {
		const std::scoped_lock lock(clientMutex_);
		return client_ != nullptr ? client_->get_client_time(serverUs) : 0;
	}

	// SendspinNetworkProvider. Nothing to wait for: the library looks a host up when it dials, and a listener binds now.
	bool is_network_ready() override { return true; }

	// PlayerRoleListener. The library's sync thread; up to 20 ms. A partial write is whole frames.
	std::size_t on_audio_write(std::uint8_t* data, std::size_t length, std::uint32_t timeout_ms) override {
		if (!streamAsked_.load(std::memory_order_acquire)) {
			// A format not listed breaks the spec: the sink stays full, and the library closes the connection when its
			// ring pins. This keeps the thread from spinning until then, and answers a stop.
			pollfd stopping{stop_.fd(), POLLIN, 0};
			::poll(&stopping, 1, static_cast<int>(timeout_ms));
			return 0;
		}
		const auto frames =
			static_cast<std::uint32_t>(std::min<std::size_t>(length / playerBytesPerFrame, feed_.capacityFrames()));
		pcm::toFloat(data, std::size_t{frames} * nodeChannels, playerBytesPerSample, scratch_.data());
		return feed_.write(scratch_, frames, std::chrono::milliseconds(timeout_ms)) * playerBytesPerFrame;
	}

	void on_stream_start() override {
		const ::sendspin::ServerPlayerStreamObject& stream = player_->get_current_stream_params();
		if (!stream.is_complete()) return;
		const std::uint32_t bits = *stream.bit_depth;
		streamAsked_.store(*stream.sample_rate == format_.rate && *stream.channels == nodeChannels &&
		                       bits == playerBytesPerSample * 8,
		                   std::memory_order_release);
		streaming_ = true;
		events_.say(notes::stream(codecName(*stream.codec), *stream.sample_rate, *stream.channels, bits));
	}

	/// The spec: "MUST stop output and clear its buffers". Gone at the next cycle, and not counted played, since the
	/// library zeroed its count. Heard from stop() too, whatever was streaming.
	void on_stream_end() override {
		feed_.clear();
		if (std::exchange(streaming_, false)) events_.say(notes::streamEnded());
	}

	// SendspinClientListener. The worker. Pairing is the connection's domain: notes, never node states.
	void on_pairing_succeeded(const std::string& server_id) override { events_.say(notes::paired(server_id)); }

	/// Only a dynamic code is offered, so a mismatch reaches here when the library ended the attempt at its round limit
	/// (`connection_manager_pairing.cpp`), which holds later attempts back for the client's life.
	void on_pairing_failed(const std::string&, ::sendspin::SendspinPairAbortReason reason) override {
		events_.say(notes::pairingFailed(abortReasonName(reason)));
		if (reason == ::sendspin::SendspinPairAbortReason::PAIRING_CODE_MISMATCH) events_.say(notes::pairingHeld());
	}
	void on_display_pairing_code(const std::string& code, ::sendspin::SendspinPairingCodeFormat) override {
		events_.say(notes::pairingCode(code));
	}
	void on_clear_pairing_code() override { events_.say(notes::pairingCodeCleared()); }
	/// A dynamic code asks for this only for an attempt the round limit held back, and ear offers no gesture to confirm
	/// it: the remedy (docs/node-types.md#failures) is a new client.
	void on_open_pairing_window() override { events_.say(notes::pairingHeld()); }

	// SourceRoleListener. The worker.
	void on_streaming_started() override {
		toServer_.streaming(true);
		events_.say(notes::sending(format_.rate, nodeChannels, sourceBits));
	}
	void on_streaming_stopped() override {
		toServer_.streaming(false);
		events_.say(notes::sendingStopped());
	}

	// SourceSink. The handoff's thread.
	void writeAudio(const std::uint8_t* pcm, std::size_t bytes, std::int64_t stampUs) override {
		source_->write_audio(pcm, bytes, stampUs);
	}

private:
	void run() noexcept {
		nameThread("ear-sendspin ", node_);
		std::unique_ptr<NodeKeyStore> keys;
		std::unique_ptr<::sendspin::SendspinClient> client;
		try {
			auto opened = NodeKeyStore::open(folder_, node_, events_);
			if (opened) {
				keys = std::move(*opened);
				client = makeClient(*keys);
				if (client->start()) {
					events_.say(notes::id(client->client_id()));
					notifier_ = std::thread([this] { notify(); });
					const std::unique_ptr<Advertiser> advertiser =
						settings_.server ? nullptr : startListening(client->client_id());
					loop(*client, advertiser.get());
				} else {
					events_.lost("the Sendspin client did not start");
				}
			} else {
				events_.lost(opened.error()); // no new identity is minted over a key file it cannot trust
			}
		} catch (const std::exception& error) {
			events_.lost(std::string("the Sendspin client died: ") + error.what());
		}
		waitForStop();
		finish(std::move(client), std::move(keys));
	}

	std::unique_ptr<::sendspin::SendspinClient> makeClient(NodeKeyStore& keys) {
		::sendspin::SendspinClient::set_log_level(::sendspin::LogLevel::WARN);
		::sendspin::SendspinClientConfig config;
		config.name = settings_.name;
		config.product_name = "ear";
		// Per node: a dialling node must not be reachable by anything else.
		config.accept_inbound_connections = !settings_.server.has_value();
		config.server_port = settings_.port;
		// Room for sockets that prove nothing to give way to newcomers, not to refuse them
		// (docs/node-types.md#on-an-untrusted-network); the transport holds one address to 8 of these and all of them
		// to twice this.
		config.server_max_connections = 32;
		// His Permission answer: a code on a status line, typed into Music Assistant.
		config.pairing_code_out_channels = {::sendspin::SendspinPairingCodeChannel::DISPLAY};
		config.pairing_code_formats = {::sendspin::SendspinPairingCodeFormat::DIGITS};
		auto client = std::make_unique<::sendspin::SendspinClient>(std::move(config));

		// The graph frequency only: the server resamples (spec "player@v1").
		::sendspin::PlayerRoleConfig playerConfig;
		using Codec = ::sendspin::SendspinCodecFormat;
		playerConfig.audio_formats = {{Codec::FLAC, nodeChannels, format_.rate, playerBytesPerSample * 8}};
		playerConfig.required_lead_time_ms = requiredLeadMs(feed_.capacityFrames(), format_.maxBlock, format_.rate);
		player_ = &client->add_player(std::move(playerConfig));
		player_->set_listener(this);
		player_->set_volume_adjustable(false); // deferred
		player_->set_mute_adjustable(false);

		// The graph frequency, PCM 24 stereo, 20 ms chunks, the library's own defaults for the rest.
		::sendspin::SourceRoleConfig sourceConfig;
		sourceConfig.sample_rate = format_.rate;
		sourceConfig.channels = nodeChannels;
		sourceConfig.bit_depth = sourceBits;
		sourceConfig.codec = Codec::PCM;
		sourceConfig.chunk_duration_ms = 20;
		source_ = &client->add_source(sourceConfig);
		source_->set_listener(this);

		client->set_listener(this);
		client->set_network_provider(this);
		client->set_persistence_provider(&keys);
		client->set_unpaired_access_enabled(false); // no roles and no audio before pairing
		{
			const std::scoped_lock lock(clientMutex_);
			client_ = client.get();
		}
		return client;
	}

	/// The library is listening by now. Its advertiser, or nothing with `advertise-failed`: the node still listens, and
	/// an operator can give the server its URL.
	std::unique_ptr<Advertiser> startListening(std::string_view clientId) {
		std::string instance = advertisedInstance(node_, clientId);
		events_.say(notes::listening(settings_.port, instance));
		auto advertiser = Advertiser::open(std::move(instance), settings_.name, settings_.port);
		if (!advertiser) {
			events_.say(notes::advertiseFailed(advertiser.error()));
			return nullptr;
		}
		return std::move(*advertiser);
	}

	/// Until stop: the library's callbacks, then the watch's reading of what it says, then any question on the network.
	void loop(::sendspin::SendspinClient& client, Advertiser* advertiser) {
		ConnectionWatch watch(settings_.server.has_value());
		bool saidDialling = false;
		pollfd stopping{stop_.fd(), POLLIN, 0};
		while (::poll(&stopping, 1, loopPollMs) == 0 || (stopping.revents & POLLIN) == 0) {
			client.loop();
			const bool paired = client.is_paired();
			const bool connected = client.is_connected();
			const auto told = watch.read({paired, connected}, std::chrono::steady_clock::now());
			if (told.disconnected) events_.say(notes::disconnected());
			if (told.connected) {
				saidDialling = false;
				const auto server = client.get_server_information();
				events_.say(notes::connected(server ? server->name : "", paired));
			}
			if (told.dial) {
				if (!std::exchange(saidDialling, true)) events_.say(notes::dialling(settings_.server->text()));
				client.connect_to(settings_.server->text());
			}
			if (advertiser != nullptr) advertiser->answer();
		}
	}

	void waitForStop() const noexcept {
		pollfd stopping{stop_.fd(), POLLIN, 0};
		while (::poll(&stopping, 1, -1) < 0 && errno == EINTR) {
		}
	}

	/// Tells the library what played, one call per bell. Apart from the worker, whose writes sync to disk: a late
	/// notify can cost a hard sync.
	void notify() noexcept {
		nameThread("ear-notify ", node_);
		std::uint64_t reported = 0;
		pollfd rung{bell_.fd(), POLLIN, 0};
		for (;;) {
			if (::poll(&rung, 1, -1) < 0) continue;
			bell_.drain();
			if (notifierStops_.load(std::memory_order_acquire)) return;
			const std::optional<PlayProgress> progress = progress_.read();
			if (!progress || progress->totalFrames == reported) continue;
			player_->notify_audio_played(static_cast<std::uint32_t>(progress->totalFrames - reported),
			                             progress->finishRawNs / 1000);
			reported = progress->totalFrames;
		}
	}

	/// The block is gone by now, so nothing rings the bell, and the handoff has closed, so nothing writes audio. The
	/// client goes last of the threads that use it, the key file after the client. `stopped` is the last thing this
	/// thread touches of anything the insides hold.
	void finish(std::unique_ptr<::sendspin::SendspinClient> client, std::unique_ptr<NodeKeyStore> keys) noexcept {
		notifierStops_.store(true, std::memory_order_release);
		bell_.ring();
		if (notifier_.joinable()) notifier_.join();
		try {
			if (client) client->stop();
			{
				const std::scoped_lock lock(clientMutex_);
				client_ = nullptr;
			}
			client.reset();
			keys.reset();
		} catch (const std::exception&) {
			// The node is going; nothing is left to tell.
		}
		std::move_only_function<void()> stopped;
		{
			const std::scoped_lock lock(clientMutex_);
			stopped = std::move(stopped_);
		}
		if (stopped) stopped();
	}

	std::shared_ptr<const folder::ConfinedFolder> folder_;
	NodeName node_;
	SendspinSettings settings_;
	AudioFormat format_;
	Feed& feed_;
	NoteGate events_;            // every note of the node's goes through it
	std::vector<float> scratch_; // the sync thread's; a feed's worth of floats
	EventFd bell_;
	EventFd stop_;
	Mailbox<PlayProgress> progress_;
	Mailbox<CaptureAnchor> capture_;
	ToServer toServer_;
	std::atomic<bool> streamAsked_{false}; // the stream's format is the one the node listed
	std::atomic<bool> notifierStops_{false};
	bool streaming_ = false;                   // worker only
	::sendspin::PlayerRole* player_ = nullptr; // set before the notifier or any callback starts
	::sendspin::SourceRole* source_ = nullptr; // set before any callback starts, and outlives every consume()
	mutable std::mutex clientMutex_;
	::sendspin::SendspinClient* client_ = nullptr; // for clientTimeOf; guarded by clientMutex_
	std::move_only_function<void()> stopped_;      // guarded by clientMutex_
	std::thread notifier_;                         // worker only
	std::thread worker_;
};

std::uint16_t requiredLeadMs(std::uint32_t feedFrames, std::uint32_t maxBlock, std::uint32_t rate) noexcept {
	// Whole milliseconds rounded up, so the lead is never short of what the frames last (docs/engine.md#dating).
	constexpr std::int64_t nsPerMs = 1'000'000;
	const std::int64_t ns = FrameClock(rate).ns(std::int64_t{feedFrames} + maxBlock);
	return static_cast<std::uint16_t>(std::min<std::int64_t>((ns + nsPerMs - 1) / nsPerMs, UINT16_MAX));
}

SendspinWorker::SendspinWorker(std::shared_ptr<const folder::ConfinedFolder> folder, NodeName node,
                               SendspinSettings settings, AudioFormat format, Feed& feed, InsidesEvents& events)
	: impl_(std::make_unique<Impl>(std::move(folder), std::move(node), std::move(settings), format, feed, events)) {}

SendspinWorker::~SendspinWorker() = default;

void SendspinWorker::stop(std::move_only_function<void()> stopped) { impl_->stop(std::move(stopped)); }

const EventFd& SendspinWorker::bell() const noexcept { return impl_->bell(); }

Mailbox<PlayProgress>& SendspinWorker::progress() noexcept { return impl_->progress(); }

HandoffConsumer& SendspinWorker::toServer() noexcept { return impl_->toServer(); }

Mailbox<CaptureAnchor>& SendspinWorker::capture() noexcept { return impl_->capture(); }

std::int64_t SendspinWorker::clientTimeOf(std::int64_t serverUs) const { return impl_->clientTimeOf(serverUs); }

} // namespace ear::sendspin
