// The library's client behind two threads (docs/design/sendspin.md sections 6, 7 and 10). The worker is what the
// library calls its main loop: start, loop, stop and destruction, every listener callback but on_audio_write, and the
// connection watch. It waits in poll() on its stop signal, 10 ms at most: the library has no wake hook for its inbox
// and no event for a lost connection or a failed dial (section 5), so this timer stays inside this adapter.
#include "worker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "connection_watch.hpp"
#include "notes.hpp"
#include "pcm.hpp"
#include "sendspin/client.h"
#include "sendspin/player_role.h"

namespace ear::sendspin {
namespace {

constexpr int loopPollMs = 10;
constexpr std::uint32_t channelCount = 2;

int makeEventFd() {
	const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fd < 0) throw std::system_error(errno, std::generic_category(), "eventfd");
	return fd;
}

void ring(int fd) noexcept {
	const std::uint64_t one = 1;
	[[maybe_unused]] const auto ignored = ::write(fd, &one, sizeof one);
}

void nameThread(std::string_view prefix, std::string_view node) {
	std::string name(prefix);
	name += node;
	name.resize(std::min<std::size_t>(name.size(), 15));   // the kernel's limit, without its terminator
	::pthread_setname_np(::pthread_self(), name.c_str());
}

std::string_view codecName(::sendspin::SendspinCodecFormat codec) noexcept {
	switch (codec) {
	case ::sendspin::SendspinCodecFormat::FLAC: return "flac";
	case ::sendspin::SendspinCodecFormat::PCM: return "pcm";
	default: return "opus";
	}
}

} // namespace

class SendspinWorker::Impl final : public ::sendspin::PlayerRoleListener, public ::sendspin::SendspinNetworkProvider {
public:
	Impl(SendspinSettings settings, AudioFormat format, Feed& feed, InsidesEvents& events, std::uint32_t feedFrames)
	    : settings_(std::move(settings)), format_(format), feed_(feed), events_(events), maxFrames_(feedFrames),
	      scratch_(static_cast<std::size_t>(feedFrames) * channelCount), bell_(makeEventFd()), stop_(makeEventFd()) {
		worker_ = std::thread([this] { run(); });
	}

	~Impl() override {
		// Without stop() (a start that went wrong after this was made) the worker is asked here.
		ring(stop_);
		if (worker_.joinable()) worker_.join();
		::close(bell_);
		::close(stop_);
	}

	void stop(std::move_only_function<void()> stopped) {
		{
			const std::scoped_lock lock(clientMutex_);
			stopped_ = std::move(stopped);
		}
		ring(stop_);
	}

	int bell() const noexcept { return bell_; }
	Mailbox<PlayProgress>& progress() noexcept { return progress_; }

	std::int64_t clientTimeOf(std::int64_t serverUs) const {
		const std::scoped_lock lock(clientMutex_);
		return client_ != nullptr ? client_->get_client_time(serverUs) : 0;
	}

	// SendspinNetworkProvider. A dialling node has nothing to wait for: the library looks the host up when it dials.
	bool is_network_ready() override { return true; }

	// PlayerRoleListener. The library's sync thread; up to 20 ms. A partial write is whole frames.
	std::size_t on_audio_write(std::uint8_t* data, std::size_t length, std::uint32_t timeout_ms) override {
		const std::uint32_t bytes = bytesPerSample_.load(std::memory_order_acquire);
		if (bytes == 0) {
			// A format not listed breaks the spec: the sink stays full, and the library closes the connection when its
			// ring pins (section 10). This keeps the thread from spinning until then, and answers a stop.
			pollfd stopping{stop_, POLLIN, 0};
			::poll(&stopping, 1, static_cast<int>(timeout_ms));
			return 0;
		}
		const std::size_t bytesPerFrame = std::size_t{bytes} * channelCount;
		const auto frames = static_cast<std::uint32_t>(std::min<std::size_t>(length / bytesPerFrame, maxFrames_));
		pcm::toFloat(data, std::size_t{frames} * channelCount, bytes, scratch_.data());
		return feed_.write(scratch_, frames, std::chrono::milliseconds(timeout_ms)) * bytesPerFrame;
	}

	void on_stream_start() override {
		const ::sendspin::ServerPlayerStreamObject& stream = player_->get_current_stream_params();
		if (!stream.is_complete()) return;
		const std::uint32_t bits = *stream.bit_depth;
		const bool asked = *stream.sample_rate == format_.rate && *stream.channels == channelCount && (bits == 16 || bits == 24);
		bytesPerSample_.store(asked ? bits / 8 : 0, std::memory_order_release);
		streaming_ = true;
		events_.say(notes::stream(codecName(*stream.codec), *stream.sample_rate, *stream.channels, bits));
	}

	/// The spec: "MUST stop output and clear its buffers". Gone at the next cycle, and not counted played, since the
	/// library zeroed its count. Heard from stop() too, whatever was streaming.
	void on_stream_end() override {
		feed_.clear();
		if (std::exchange(streaming_, false)) events_.say(notes::streamEnded());
	}

private:
	void run() noexcept {
		nameThread("ear-sendspin ", settings_.name);
		std::unique_ptr<::sendspin::SendspinClient> client;
		try {
			client = makeClient();
			if (client->start()) {
				notifier_ = std::thread([this] { notify(); });
				loop(*client);
			} else {
				events_.lost("the Sendspin client did not start");
			}
		} catch (const std::exception& error) {
			events_.lost(std::string("the Sendspin client died: ") + error.what());
		}
		waitForStop();
		finish(std::move(client));
	}

	std::unique_ptr<::sendspin::SendspinClient> makeClient() {
		::sendspin::SendspinClient::set_log_level(::sendspin::LogLevel::WARN);
		::sendspin::SendspinClientConfig config;
		config.name = settings_.name;
		config.product_name = "ear";
		config.accept_inbound_connections = false;   // it dials: nothing else may reach it
		auto client = std::make_unique<::sendspin::SendspinClient>(std::move(config));

		// The graph frequency only: the server resamples (R5, spec "player@v1").
		::sendspin::PlayerRoleConfig playerConfig;
		using Codec = ::sendspin::SendspinCodecFormat;
		playerConfig.audio_formats = {{Codec::FLAC, channelCount, format_.rate, 24},
		                              {Codec::FLAC, channelCount, format_.rate, 16},
		                              {Codec::PCM, channelCount, format_.rate, 16}};
		player_ = &client->add_player(std::move(playerConfig));
		player_->set_listener(this);
		player_->set_volume_adjustable(false);   // deferred (section 2)
		player_->set_mute_adjustable(false);
		client->set_network_provider(this);
		client->set_unpaired_access_enabled(true);   // the test's server approves the client until pairing arrives
		{
			const std::scoped_lock lock(clientMutex_);
			client_ = client.get();
		}
		return client;
	}

	/// Until stop: the library's callbacks, then the watch's reading of what it says.
	void loop(::sendspin::SendspinClient& client) {
		ConnectionWatch watch(settings_.server.has_value());
		bool saidDialling = false;
		pollfd stopping{stop_, POLLIN, 0};
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
		}
	}

	void waitForStop() const noexcept {
		pollfd stopping{stop_, POLLIN, 0};
		while (::poll(&stopping, 1, -1) < 0 && errno == EINTR) {}
	}

	/// Tells the library what played, one call per bell. Apart from the worker, whose writes sync to disk: a late
	/// notify can cost a hard sync (section 6).
	void notify() noexcept {
		nameThread("ear-notify ", settings_.name);
		std::uint64_t reported = 0;
		pollfd rung{bell_, POLLIN, 0};
		for (;;) {
			if (::poll(&rung, 1, -1) < 0) continue;
			std::uint64_t rings = 0;
			[[maybe_unused]] const auto ignored = ::read(bell_, &rings, sizeof rings);
			if (notifierStops_.load(std::memory_order_acquire)) return;
			const std::optional<PlayProgress> progress = progress_.read();
			if (!progress || progress->totalFrames == reported) continue;
			player_->notify_audio_played(static_cast<std::uint32_t>(progress->totalFrames - reported),
			                             progress->finishRawNs / 1000);
			reported = progress->totalFrames;
		}
	}

	/// The block is gone by now, so nothing rings the bell. The client goes last of the threads that use it. `stopped`
	/// is the last thing this thread touches of anything the insides hold.
	void finish(std::unique_ptr<::sendspin::SendspinClient> client) noexcept {
		notifierStops_.store(true, std::memory_order_release);
		ring(bell_);
		if (notifier_.joinable()) notifier_.join();
		try {
			if (client) client->stop();
			{
				const std::scoped_lock lock(clientMutex_);
				client_ = nullptr;
			}
			client.reset();
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

	SendspinSettings settings_;
	AudioFormat format_;
	Feed& feed_;
	InsidesEvents& events_;
	std::uint32_t maxFrames_;
	std::vector<float> scratch_;   // the sync thread's; a feed's worth of floats
	int bell_;
	int stop_;
	Mailbox<PlayProgress> progress_;
	std::atomic<std::uint32_t> bytesPerSample_{0};   // 0: nothing the node asked for is streaming
	std::atomic<bool> notifierStops_{false};
	bool streaming_ = false;                         // worker only
	::sendspin::PlayerRole* player_ = nullptr;       // set before the notifier or any callback starts
	mutable std::mutex clientMutex_;
	::sendspin::SendspinClient* client_ = nullptr;   // for clientTimeOf; guarded by clientMutex_
	std::move_only_function<void()> stopped_;        // guarded by clientMutex_
	std::thread notifier_;                           // worker only
	std::thread worker_;
};

SendspinWorker::SendspinWorker(SendspinSettings settings, AudioFormat format, Feed& feed, InsidesEvents& events,
                               std::uint32_t feedFrames)
    : impl_(std::make_unique<Impl>(std::move(settings), format, feed, events, feedFrames)) {}

SendspinWorker::~SendspinWorker() = default;

void SendspinWorker::stop(std::move_only_function<void()> stopped) { impl_->stop(std::move(stopped)); }

int SendspinWorker::bell() const noexcept { return impl_->bell(); }

Mailbox<PlayProgress>& SendspinWorker::progress() noexcept { return impl_->progress(); }

std::int64_t SendspinWorker::clientTimeOf(std::int64_t serverUs) const { return impl_->clientTimeOf(serverUs); }

} // namespace ear::sendspin
