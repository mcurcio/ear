// The library's client on the node's two loops (docs/node-types.md#threads). The worker is what the library calls its
// main loop: start, loop, stop and destruction, every listener callback but on_audio_write, the connection watch, and a
// listening node's mDNS, all on its dedicated loop's 10 ms tick: the library has no wake hook for its inbox and no
// event for a lost connection or a failed dial, so the timer stays inside this adapter.
#include "worker.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

#include "advertiser.hpp"
#include "notes.hpp"
#include "pcm.hpp"

namespace ear::nodes::sendspin {
namespace {

constexpr std::chrono::milliseconds tickPeriod{10};

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

void PlayerSlot::set(::sendspin::PlayerRole* player) noexcept {
	const std::scoped_lock lock(mutex_);
	player_ = player;
}

PlayerSlot::Told PlayerSlot::notify(const SendspinClock& clock, const PlayProgress& progress, std::uint64_t reported) {
	const std::scoped_lock lock(mutex_);
	const std::uint64_t frames = framesToTell(progress, reported, clock.stream());
	if (frames == 0) return Told::nothing;
	if (player_ == nullptr) return Told::noPlayer;
	player_->notify_audio_played(static_cast<std::uint32_t>(frames), progress.finishRawNs / 1000);
	return Told::told;
}

void PlayerSlot::endStream(SendspinClock& clock) {
	const std::scoped_lock lock(mutex_);
	clock.clear();
}

Worker::Worker(WorkerParts parts)
	: toServer(parts.format.rate, *this), handle_(parts.handle), keys_(parts.keys), clock_(parts.clock),
	  writer_(parts.writer), player_(parts.player), self_(parts.self), settings_(std::move(parts.settings)),
	  format_(parts.format), scratch_(std::size_t{parts.writer.capacityFrames()} * nodeChannels) {}

Worker::~Worker() {
	// The block is gone or silent by now, so no report the clock wakes the notifier for matters, and the sink has
	// finished, so nothing writes audio. The notifier is cut off from the player before the client goes; the client goes last of what uses
	// it, the key store after the client.
	tick_.reset();
	advertiser_.reset(); // its mDNS goodbye
	player_.set(nullptr);
	try {
		if (client_) client_->stop(); // the Sendspin goodbye; its threads joined
		std::unique_ptr<::sendspin::SendspinClient> client;
		{
			const std::scoped_lock lock(clientMutex_);
			client = std::move(client_);
		}
		client.reset();
		keyStore_.reset();
	} catch (const std::exception&) {
		// The configuration is going; nothing is left to tell.
	}
}

void Worker::begin() {
	tick_ = self_.every(tickPeriod, [this] { tick(); });
}

std::int64_t Worker::clientTimeOf(std::int64_t serverUs) const {
	const std::scoped_lock lock(clientMutex_);
	return client_ != nullptr ? client_->get_client_time(serverUs) : 0;
}

void Worker::tick() {
	try {
		if (client_ == nullptr) startClient();
		else turn();
	} catch (const std::exception& error) {
		fail(std::string("the Sendspin client died: ") + error.what());
	}
}

void Worker::fail(std::string detail) {
	tick_.reset();
	handle_.lost(std::move(detail));
}

void Worker::startClient() {
	// The store's rule trusted the text before any of this loop's work ran, so this only fails on a bug.
	auto keys = NodeKeyStore::open(keys_, handle_);
	if (!keys) return fail(keys.error());
	keyStore_ = std::move(*keys);
	auto client = makeClient(*keyStore_);
	if (!client->start()) {
		client->stop();
		return fail("the Sendspin client did not start");
	}
	handle_.say(notes::id(client->client_id()));
	const std::string clientId = client->client_id();
	{
		const std::scoped_lock lock(clientMutex_);
		client_ = std::move(client);
	}
	player_.set(playerRole_);
	watch_.emplace(settings_.server.has_value());
	if (!settings_.server) startListening(clientId);
	turn();
}

std::unique_ptr<::sendspin::SendspinClient> Worker::makeClient(NodeKeyStore& keys) {
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
	playerConfig.audio_formats = {{Codec::FLAC, nodeChannels, format_.rate.hz(), playerBytesPerSample * 8}};
	playerConfig.required_lead_time_ms = requiredLeadMs(writer_.capacityFrames(), format_.maxBlock, format_.rate);
	playerConfig.audio_buffer_capacity = playerBufferBytes; // what the clock's undrained bound is derived from
	playerRole_ = &client->add_player(std::move(playerConfig));
	playerRole_->set_listener(this);
	playerRole_->set_volume_adjustable(false); // deferred
	playerRole_->set_mute_adjustable(false);

	// The graph frequency, PCM 24 stereo, 20 ms chunks, the library's own defaults for the rest.
	::sendspin::SourceRoleConfig sourceConfig;
	sourceConfig.sample_rate = format_.rate.hz();
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
	return client;
}

/// The library is listening by now. Its advertiser, or none with `advertise-failed`: the node still listens, and an
/// operator can give the server its URL.
void Worker::startListening(std::string_view clientId) {
	std::string instance = advertisedInstance(handle_.node(), clientId);
	handle_.say(notes::listening(settings_.port, instance));
	auto advertiser =
		ear::mdns::Advertiser::open(ear::mdns::Listing{.service = "_sendspin._tcp",
	                                                   .instance = instance,
	                                                   .host = instance,
	                                                   .port = settings_.port,
	                                                   .txt = {"path=/sendspin", "name=" + settings_.name}});
	if (!advertiser) {
		handle_.say(notes::advertiseFailed(advertiser.error()));
		return;
	}
	advertiser_ = std::move(*advertiser);
	askedInstance_ = std::move(instance);
}

/// One turn: the library's callbacks, then the watch's reading of what it says, then any question on the network.
void Worker::turn() {
	::sendspin::SendspinClient& client = *client_;
	client.loop();
	const bool paired = client.is_paired();
	const bool connected = client.is_connected();
	const auto told = watch_->read({paired, connected}, std::chrono::steady_clock::now());
	if (told.disconnected) handle_.say(notes::disconnected());
	if (told.connected) {
		saidDialling_ = false;
		const auto server = client.get_server_information();
		handle_.say(notes::connected(server ? server->name : "", paired));
	}
	if (told.dial) {
		if (!std::exchange(saidDialling_, true)) handle_.say(notes::dialling(settings_.server->text()));
		client.connect_to(settings_.server->text());
	}
	if (advertiser_ == nullptr) return;
	// Probed, then announced (docs/node-types.md#mdns): a rename only if another responder held its instance.
	auto mdns = advertiser_->turn();
	if (mdns.announced && mdns.announced->instance != askedInstance_)
		handle_.say(notes::renamed(askedInstance_, mdns.announced->instance));
	if (mdns.failed) {
		handle_.say(notes::advertiseFailed(*mdns.failed));
		advertiser_.reset();
	}
}

// PlayerRoleListener. The library's sync thread; up to 20 ms. A partial write is whole frames.
std::size_t Worker::on_audio_write(std::uint8_t* data, std::size_t length, std::uint32_t timeout_ms) {
	if (!streamAsked_.load(std::memory_order_acquire)) {
		// A format not listed breaks the spec: the sink stays full, and the library closes the connection when its
		// ring pins. This keeps the thread from spinning until then, the source idle meanwhile.
		writer_.idle(std::chrono::milliseconds(timeout_ms));
		return 0;
	}
	const auto frames =
		static_cast<std::uint32_t>(std::min<std::size_t>(length / playerBytesPerFrame, writer_.capacityFrames()));
	pcm::toFloat(data, std::size_t{frames} * nodeChannels, playerBytesPerSample, scratch_.data());
	return clock_.write(scratch_, frames, std::chrono::milliseconds(timeout_ms)) * playerBytesPerFrame;
}

void Worker::on_stream_start() {
	const ::sendspin::ServerPlayerStreamObject& stream = playerRole_->get_current_stream_params();
	if (!stream.is_complete()) return;
	const std::uint32_t bits = *stream.bit_depth;
	streamAsked_.store(*stream.sample_rate == format_.rate.hz() && *stream.channels == nodeChannels &&
	                       bits == playerBytesPerSample * 8,
	                   std::memory_order_release);
	streaming_ = true;
	handle_.say(notes::stream(codecName(*stream.codec), *stream.sample_rate, *stream.channels, bits));
}

/// The spec: "MUST stop output and clear its buffers". Gone at the next cycle, and not counted played, since the
/// library zeroed its count. Heard from stop() too, whatever was streaming.
void Worker::on_stream_end() {
	player_.endStream(clock_);
	if (std::exchange(streaming_, false)) handle_.say(notes::streamEnded());
}

void Worker::on_pairing_succeeded(const std::string& server_id) { handle_.say(notes::paired(server_id)); }

/// Only a dynamic code is offered, so a mismatch reaches here when the library ended the attempt at its round limit
/// (`connection_manager_pairing.cpp`), which holds later attempts back for the client's life.
void Worker::on_pairing_failed(const std::string&, ::sendspin::SendspinPairAbortReason reason) {
	handle_.say(notes::pairingFailed(abortReasonName(reason)));
	if (reason == ::sendspin::SendspinPairAbortReason::PAIRING_CODE_MISMATCH) handle_.say(notes::pairingHeld());
}

void Worker::on_display_pairing_code(const std::string& code, ::sendspin::SendspinPairingCodeFormat) {
	handle_.say(notes::pairingCode(code));
}

void Worker::on_clear_pairing_code() { handle_.say(notes::pairingCodeCleared()); }

/// A dynamic code asks for this only for an attempt the round limit held back, and ear offers no gesture to confirm it:
/// the remedy (docs/node-types.md#failures) is a new client.
void Worker::on_open_pairing_window() { handle_.say(notes::pairingHeld()); }

void Worker::on_streaming_started() {
	toServer.streaming(true);
	handle_.say(notes::sending(format_.rate.hz(), nodeChannels, sourceBits));
}

void Worker::on_streaming_stopped() {
	toServer.streaming(false);
	handle_.say(notes::sendingStopped());
}

void Worker::writeAudio(const std::uint8_t* pcm, std::size_t bytes, std::int64_t stampUs) {
	source_->write_audio(pcm, bytes, stampUs);
}

Notifier::Notifier(Loop& self, SendspinClock& clock, PlayerSlot& player) noexcept
	: self_(self), clock_(clock), player_(player) {}

void Notifier::begin() {
	watch_ = self_.onWake(clock_.wake(), [this] { notify(); });
}

/// The frames counted since the last report of this stream (none of a stream the library forgot at its end, judged
/// under the lock its end takes), dated by the clock's newest finish. Before the client started nothing is told and
/// nothing counted told: the library learns it all at its first report.
void Notifier::notify() {
	const std::optional<PlayProgress> progress = clock_.progress();
	if (!progress) return;
	if (player_.notify(clock_, *progress, reported_) != PlayerSlot::Told::told) return;
	if (lastFinish_ && progress->finishRawNs < *lastFinish_) steppedBack_.fetch_add(1, std::memory_order_acq_rel);
	lastFinish_ = progress->finishRawNs;
	reported_ = progress->played;
}

std::uint16_t requiredLeadMs(std::uint32_t sourceFrames, std::uint32_t maxBlock, SampleRate rate) noexcept {
	// Whole milliseconds rounded up, so the lead is never short of what the frames last (docs/engine.md#dating).
	constexpr std::int64_t nsPerMs = 1'000'000;
	const std::int64_t ns = FrameClock(rate).ns(std::int64_t{sourceFrames} + maxBlock);
	return static_cast<std::uint16_t>(std::min<std::int64_t>((ns + nsPerMs - 1) / nsPerMs, UINT16_MAX));
}

} // namespace ear::nodes::sendspin
