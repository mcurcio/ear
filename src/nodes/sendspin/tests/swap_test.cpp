// What a sendspin node's change in place rests on (docs/node-types.md#changes-in-place): the library's client stopped,
// destroyed and made again inside one process, over the same key store, against the real server
// (test/support/sendspin_server.py). The new client has the old one's client id and pairings, read from the key
// store's memory with no read of the file; and how long the swap takes, from the old client's stop() to the new one's
// start() returning, whatever the old one was doing: connected and streaming, listening with a server dialled in, or
// dialling a peer that accepted the socket and never answers the upgrade. Every wait is for an event; a deadline only
// fails the test.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/node_handle.hpp"
#include "ear/core/store.hpp"
#include "key_store.hpp"
#include "sendspin/client.h"
#include "sendspin/player_role.h"
#include "sendspin_server.hpp"
#include "sleeps.hpp"

namespace {

using namespace ear;
using namespace ear::nodes::sendspin;
using namespace std::chrono_literals;
using ear::test::SendspinTestServer;
using Clock = std::chrono::steady_clock;

/// The key file in memory, counting the reads of its text: NodeKeyStore::open reads it once.
class CountingStore final : public Store {
public:
	const std::string& fileName() const noexcept override { return name_; }
	std::string text() const override {
		const std::scoped_lock lock(mutex_);
		++reads_;
		return text_;
	}
	StoreWritten replace(std::string text) override {
		const std::scoped_lock lock(mutex_);
		text_ = std::move(text);
		return {};
	}
	void replaceLater(std::string text, Loop& loop, std::move_only_function<void(StoreWritten)> done) override {
		static_cast<void>(replace(std::move(text)));
		loop.post([done = std::move(done)]() mutable { done({}); });
	}
	int reads() const {
		const std::scoped_lock lock(mutex_);
		return reads_;
	}

private:
	const std::string name_ = "sendspin-swap.key";
	mutable std::mutex mutex_;
	mutable int reads_ = 0;
	std::string text_ = emptyKeyRecord();
};

class Handle final : public NodeHandle {
public:
	const NodeName& node() const noexcept override { return name_; }
	void lost(std::string) override {}
	void log(LogEntry) override {}
	void say(std::string) override {}

private:
	NodeName name_ = "swap";
};

/// The listeners a client needs, as the worker is: the pairing code shown, the audio taken and thrown away.
class Listener final : public ::sendspin::SendspinClientListener,
					   public ::sendspin::SendspinNetworkProvider,
					   public ::sendspin::PlayerRoleListener {
public:
	bool is_network_ready() override { return true; }
	std::size_t on_audio_write(std::uint8_t*, std::size_t length, std::uint32_t) override { return length; }
	void on_display_pairing_code(const std::string& shown, ::sendspin::SendspinPairingCodeFormat) override {
		const std::scoped_lock lock(mutex_);
		code_ = shown;
	}
	std::optional<std::string> code() {
		const std::scoped_lock lock(mutex_);
		return code_;
	}

private:
	std::mutex mutex_;
	std::optional<std::string> code_;
};

/// A client as Worker::makeClient makes one, over `keys`.
std::unique_ptr<::sendspin::SendspinClient> makeClient(NodeKeyStore& keys, Listener& listener,
                                                       std::optional<std::uint16_t> listenOn) {
	::sendspin::SendspinClientConfig config;
	config.name = "swap";
	config.product_name = "ear";
	config.accept_inbound_connections = listenOn.has_value();
	config.server_port = listenOn.value_or(8928);
	config.server_max_connections = 32;
	config.pairing_code_out_channels = {::sendspin::SendspinPairingCodeChannel::DISPLAY};
	config.pairing_code_formats = {::sendspin::SendspinPairingCodeFormat::DIGITS};
	auto client = std::make_unique<::sendspin::SendspinClient>(std::move(config));
	::sendspin::PlayerRoleConfig player;
	player.audio_formats = {{::sendspin::SendspinCodecFormat::FLAC, 2, 48000, 24}};
	auto& role = client->add_player(std::move(player));
	role.set_listener(&listener);
	client->set_listener(&listener);
	client->set_network_provider(&listener);
	client->set_persistence_provider(&keys);
	client->set_unpaired_access_enabled(false);
	return client;
}

/// Turns `client` as the worker's tick does, until `done` holds: whether it did within `deadline`.
bool turnUntil(::sendspin::SendspinClient& client, const std::function<bool()>& done,
               std::chrono::milliseconds deadline = 20s) {
	const auto until = Clock::now() + deadline;
	for (;;) {
		client.loop();
		if (done()) return true;
		if (Clock::now() > until) return false;
		ear::sleeps::poll(10ms, "the worker's tick: the library has no wake hook for its inbox");
	}
}

/// A port nobody listens on now, from below the kernel's outgoing range as the system tests' freeTcpPort.
std::uint16_t freePort() {
	const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(fd >= 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
	socklen_t length = sizeof address;
	REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
	::close(fd);
	return ntohs(address.sin_port);
}

/// Whether `port` on every address can be bound now: the old client let it go.
bool bindable(std::uint16_t port) {
	const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	const int on = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);
	const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0;
	::close(fd);
	return bound;
}

/// Pairs `client`, connected to `server` and unpaired, by the code it shows.
void pair(::sendspin::SendspinClient& client, Listener& listener, SendspinTestServer& server) {
	server.send("pair");
	REQUIRE(turnUntil(client, [&] { return listener.code().has_value(); }));
	server.send("code " + *listener.code());
	REQUIRE(turnUntil(client, [&] { return client.is_paired() && client.is_connected(); }));
}

/// What a swap took: the old client's stop(), its destruction, and the new one made and started.
struct Swapped {
	std::unique_ptr<::sendspin::SendspinClient> client;
	Clock::duration took;
};

Swapped swap(std::unique_ptr<::sendspin::SendspinClient> old, NodeKeyStore& keys, Listener& listener,
             std::optional<std::uint16_t> listenOn) {
	const auto began = Clock::now();
	old->stop();
	old.reset();
	auto made = makeClient(keys, listener, listenOn);
	REQUIRE(made->start());
	return {std::move(made), Clock::now() - began};
}

std::string ms(Clock::duration took) {
	return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()) + " ms";
}

/// The id the server's `connected <id>` event names.
std::string idOf(const std::string& event) { return event.substr(std::string_view("connected ").size()); }

} // namespace

TEST_CASE("A client made again over the same key store, after the first was stopped and destroyed while streaming, "
          "has its client id and pairing, reads no file, and the swap takes well under the change's deadline",
          "[sendspin][swap]") {
	SendspinTestServer server;
	CountingStore store;
	Handle handle;
	auto opened = NodeKeyStore::open(store, handle);
	REQUIRE(opened.has_value());
	NodeKeyStore& keys = **opened;
	const int readsAtOpen = store.reads();
	Listener listener;

	auto first = makeClient(keys, listener, std::nullopt);
	REQUIRE(first->start());
	const std::string id = first->client_id();
	first->connect_to(server.url());
	REQUIRE(turnUntil(*first, [&] { return first->is_connected(); }));
	pair(*first, listener, server);
	const auto firstConnected = server.waitForEvent("connected ", 5s);
	REQUIRE(firstConnected);
	CHECK(idOf(*firstConnected) == id);
	server.send("play 30");
	REQUIRE(server.waitForEvent("play-start ", 20s));

	auto [second, took] = swap(std::move(first), keys, listener, std::nullopt);
	INFO("the swap took " << ms(took));
	CHECK(took < 1s);
	CHECK(second->client_id() == id);
	second->connect_to(server.url());
	// Paired with no new attempt: the long-term PSK the key store keeps in memory.
	REQUIRE(turnUntil(*second, [&] { return second->is_connected(); }));
	CHECK(second->is_paired());
	const auto secondConnected = server.waitForEvent("connected ", 5s);
	REQUIRE(secondConnected);
	CHECK(idOf(*secondConnected) == id);
	CHECK(store.reads() == readsAtOpen); // the identity and the records came from memory
	second->stop();
}

TEST_CASE("A listening client swapped for one on another port lets its port go in the stop, and the new one is "
          "dialled on its new port, paired",
          "[sendspin][swap]") {
	const std::uint16_t oldPort = freePort();
	const std::uint16_t newPort = freePort();
	SendspinTestServer server;
	CountingStore store;
	Handle handle;
	auto opened = NodeKeyStore::open(store, handle);
	REQUIRE(opened.has_value());
	NodeKeyStore& keys = **opened;
	Listener listener;

	auto first = makeClient(keys, listener, oldPort);
	REQUIRE(first->start());
	const std::string id = first->client_id();
	server.send("dial ws://127.0.0.1:" + std::to_string(oldPort) + "/sendspin");
	REQUIRE(turnUntil(*first, [&] { return first->is_connected(); }));
	pair(*first, listener, server);

	auto [second, took] = swap(std::move(first), keys, listener, newPort);
	INFO("the swap took " << ms(took));
	CHECK(took < 1s);
	CHECK(bindable(oldPort));
	CHECK(second->client_id() == id);
	server.send("dial ws://127.0.0.1:" + std::to_string(newPort) + "/sendspin");
	REQUIRE(turnUntil(*second, [&] { return second->is_connected(); }));
	CHECK(second->is_paired());
	second->stop();
}

TEST_CASE("A client dialling a peer that took the socket and never answers the upgrade is swapped within the change's "
          "deadline",
          "[sendspin][swap]") {
	// A peer that accepts and then says nothing: the attempt's upgrade is in flight, which stop() leaves and the
	// destructor joins (the library's integration guide, "Stopping and Restarting").
	const int listening = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(listening >= 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	REQUIRE(::bind(listening, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
	REQUIRE(::listen(listening, 4) == 0);
	socklen_t length = sizeof address;
	REQUIRE(::getsockname(listening, reinterpret_cast<sockaddr*>(&address), &length) == 0);
	const std::uint16_t port = ntohs(address.sin_port);

	CountingStore store;
	Handle handle;
	auto opened = NodeKeyStore::open(store, handle);
	REQUIRE(opened.has_value());
	Listener listener;
	auto first = makeClient(**opened, listener, std::nullopt);
	REQUIRE(first->start());
	first->connect_to("ws://127.0.0.1:" + std::to_string(port) + "/sendspin");
	int accepted = -1;
	REQUIRE(turnUntil(*first, [&] {
		pollfd ready{listening, POLLIN, 0};
		if (::poll(&ready, 1, 0) != 1) return false;
		accepted = ::accept4(listening, nullptr, nullptr, SOCK_CLOEXEC);
		return accepted >= 0;
	}));

	auto [second, took] = swap(std::move(first), **opened, listener, std::nullopt);
	INFO("the swap took " << ms(took));
	CHECK(took < 10s);
	second->stop();
	::close(accepted);
	::close(listening);
}
