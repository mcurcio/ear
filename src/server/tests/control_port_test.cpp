// ControlPort, ControlThread and LevelBell (docs/design/batch2.md sections 4.6 and 6, R2
// criteria 1-7): real sockets on 127.0.0.1:0, a fake main loop on its own thread and a fake
// gateway that answers on it. Every step waits for the event it needs (a reply, a notification,
// a close); the client's deadline only fails a test.
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/control/rpc.hpp"
#include "ear/server/control_port.hpp"
#include "ws_client.hpp"

using namespace ear::server;
using ear::test::Closed;
using ear::test::httpRequest;
using ear::test::Refused;
using ear::test::WsClient;
using ear::test::WsOptions;
using nlohmann::json;
namespace http = boost::beast::http;

namespace {

/// The main loop: posted work runs on its own thread, in order.
class FakeMainLoop final : public ear::Executor {
public:
	FakeMainLoop()
		: thread_([this] {
			  for (;;) {
				  std::move_only_function<void()> work;
				  {
					  std::unique_lock lock(mutex_);
					  ready_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
					  if (queue_.empty()) return;
					  work = std::move(queue_.front());
					  queue_.pop_front();
				  }
				  work();
			  }
		  }) {}
	~FakeMainLoop() {
		{
			const std::scoped_lock lock(mutex_);
			stopping_ = true;
		}
		ready_.notify_one();
		thread_.join();
	}
	void post(std::move_only_function<void()> work) override {
		{
			const std::scoped_lock lock(mutex_);
			queue_.push_back(std::move(work));
		}
		ready_.notify_one();
	}
	/// Returns once everything posted before it has run.
	void drain() {
		std::promise<void> done;
		post([&] { done.set_value(); });
		done.get_future().wait();
	}

private:
	std::mutex mutex_;
	std::condition_variable ready_;
	bool stopping_ = false;
	std::deque<std::move_only_function<void()>> queue_;
	std::thread thread_;
};

/// The gateway: runs commands on the fake main loop (every edit succeeds, graph.get returns
/// `view`) and posts the replies and the test's events to the control thread in one order.
class FakeGateway final : public ear::control::GraphGateway {
public:
	FakeGateway(FakeMainLoop& mainLoop, ear::Executor& control) : mainLoop_(mainLoop), control_(control) {
		view.types = {"white"};
	}

	void run(std::vector<ear::control::Command> commands,
	         std::move_only_function<void(std::vector<ear::control::CommandResult>)> done) override {
		mainLoop_.post([this, commands = std::move(commands), done = std::move(done)]() mutable {
			if (hold) {   // the test holds this request on the main loop
				hold = false;
				held.set_value();
				release.get_future().wait();
			}
			std::vector<ear::control::CommandResult> results;
			for (const auto& command : commands) {
				if (std::holds_alternative<ear::control::GetGraph>(command)) results.emplace_back(view);
				else results.emplace_back(std::expected<void, ear::CommandError>{});
			}
			control_.post([done = std::move(done), results = std::move(results)]() mutable { done(std::move(results)); });
		});
	}

	void watch(ear::control::EventSink& sink) override {
		mainLoop_.post([this, &sink] { sink_ = &sink; });
	}

	/// A graph event, as the main loop would send it.
	void emit(ear::control::GraphEvent event) {
		mainLoop_.post([this, event = std::move(event)]() mutable {
			control_.post([sink = sink_, event = std::move(event)]() mutable { sink->graphChanged(std::move(event)); });
		});
	}

	ear::GraphView view{};
	bool hold = false;   // set before the request; read on the main loop
	std::promise<void> held;
	std::promise<void> release;

private:
	FakeMainLoop& mainLoop_;
	ear::Executor& control_;
	ear::control::EventSink* sink_ = nullptr;   // main loop only
};

class FakeLevels final : public ear::LevelReader {
public:
	std::vector<ear::NodeLevels> latestLevels() override {
		const std::scoped_lock lock(mutex);
		return levels;
	}
	std::mutex mutex;
	std::vector<ear::NodeLevels> levels;
};

std::filesystem::path makeWebRoot() {
	static int count = 0;
	const auto top = std::filesystem::temp_directory_path() /
	                 ("ear-control-port-" + std::to_string(::getpid()) + "-" + std::to_string(++count));
	std::filesystem::remove_all(top);
	std::filesystem::create_directories(top / "web" / "sub");
	std::ofstream(top / "web" / "index.html") << "<html>graph editor</html>";
	std::ofstream(top / "web" / "app.js") << "export {};";
	std::ofstream(top / "secret.txt") << "outside the root";
	return top;
}

/// One port on the control thread, with its fakes. Members are destroyed port first and the
/// control thread last, as main.cpp orders them; the main loop goes before the control thread,
/// so whatever it still posts there finds it.
struct Fixture {
	explicit Fixture(std::vector<std::string> allowedHosts = {}) {
		ControlPortSettings settings{"127.0.0.1", 0, top / "web", std::move(allowedHosts)};
		auto started = ControlPort::start(settings, control, gateway, levels, bell);
		REQUIRE(started);
		port = std::move(*started);
	}
	~Fixture() {
		try {
			gateway.release.set_value();   // a request a failed test left held
		} catch (const std::future_error&) {
		}
		port.reset();
		mainLoop.drain();
		std::filesystem::remove_all(top);
	}
	std::uint16_t number() const { return port->port(); }
	std::string host() const { return "127.0.0.1:" + std::to_string(number()); }

	std::filesystem::path top = makeWebRoot();
	ControlThread control;
	FakeMainLoop mainLoop;
	FakeGateway gateway{mainLoop, control};
	FakeLevels levels;
	LevelBell bell;
	std::unique_ptr<ControlPort> port;
};

json call(WsClient& client, int id, const std::string& method) {
	client.send(json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}}.dump());
	return json::parse(client.read());
}

/// A client subscribed to graph notifications.
void subscribe(WsClient& client) { REQUIRE(call(client, 1, "graph.subscribe").contains("result")); }

/// The receive buffer of a client that stops reading: a fixed size (Linux would grow it to
/// megabytes), so a few hundred events fill it and the port's queue behind it.
constexpr int smallWindow = 256 * 1024;

/// The settings of a large event (32 KiB).
const json bigSettings = {{"pad", std::string(32 * 1024, 'x')}};

ear::control::GraphEvent removed(const std::string& node) { return ear::control::NodeRemoved{node}; }

json removedNotification(const std::string& node) {
	return {{"jsonrpc", "2.0"}, {"method", "node.removed"}, {"params", {{"name", node}}}};
}

/// This process's descriptor table, full: the soft limit lowered and every free descriptor
/// below it taken, but `free` of them. The destructor gives them back and restores the limit.
class FullDescriptorTable {
public:
	explicit FullDescriptorTable(int free) {
		REQUIRE(::getrlimit(RLIMIT_NOFILE, &saved_) == 0);
		const int lowestFree = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
		REQUIRE(lowestFree >= 0);
		::close(lowestFree);
		rlimit lowered = saved_;
		lowered.rlim_cur = static_cast<rlim_t>(lowestFree + 64);
		REQUIRE(::setrlimit(RLIMIT_NOFILE, &lowered) == 0);
		for (int fd; (fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC)) >= 0;) taken_.push_back(fd);
		REQUIRE(errno == EMFILE);
		for (int i = 0; i < free; ++i) {
			::close(taken_.back());
			taken_.pop_back();
		}
	}
	~FullDescriptorTable() {
		for (const int fd : taken_) ::close(fd);
		::setrlimit(RLIMIT_NOFILE, &saved_);
	}
	FullDescriptorTable(const FullDescriptorTable&) = delete;
	FullDescriptorTable& operator=(const FullDescriptorTable&) = delete;

private:
	rlimit saved_{};
	std::vector<int> taken_;
};

/// A plain TCP connection to 127.0.0.1:`port` (it completes in the kernel's queue, before the
/// port accepts it); -1 if it could not be made.
int connectTcp(std::uint16_t port) {
	const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
		::close(fd);
		return -1;
	}
	return fd;
}

} // namespace

TEST_CASE("With no descriptor left the port closes the waiting connection and serves the next",
          "[control-port]") {
	Fixture fixture;
	bool closedByPort = false;
	{
		// One descriptor free: the client's socket takes it, so the port's accept finds none.
		FullDescriptorTable full(1);
		const int client = connectTcp(fixture.number());
		REQUIRE(client >= 0);
		// The port refuses it (and goes on waiting for the next connection): the client reads
		// end of file or a reset. Before the remedy the port retried at once forever and the
		// connection stayed queued, so this read reached the deadline.
		pollfd readable{client, POLLIN, 0};
		if (::poll(&readable, 1, 10'000) == 1) {   // the deadline only fails the test
			char byte = 0;
			closedByPort = ::recv(client, &byte, 1, 0) <= 0;
		}
		::close(client);
	}
	CHECK(closedByPort);
	WsClient later(fixture.number());
	CHECK(call(later, 9, "graph.get")["id"] == 9);
}

TEST_CASE("The control port serves the web root's files and refuses paths out of it", "[control-port]") {
	Fixture fixture;
	const auto index = httpRequest(fixture.number(), http::verb::get, "/", fixture.host());
	CHECK(index.status == 200);
	CHECK(index.contentType == "text/html");
	CHECK(index.body == "<html>graph editor</html>");

	const auto head = httpRequest(fixture.number(), http::verb::head, "/", fixture.host());
	CHECK(head.status == 200);
	CHECK(head.contentLength == std::to_string(std::string("<html>graph editor</html>").size()));
	CHECK(head.body.empty());

	CHECK(httpRequest(fixture.number(), http::verb::get, "/app.js", fixture.host()).contentType == "text/javascript");
	CHECK(httpRequest(fixture.number(), http::verb::get, "/missing.js", fixture.host()).status == 404);
	CHECK(httpRequest(fixture.number(), http::verb::post, "/", fixture.host()).status == 405);

	for (const std::string target : {"/../secret.txt", "/sub/../../secret.txt", "/..", "/sub/../index.html"}) {
		INFO(target);
		const auto escaped = httpRequest(fixture.number(), http::verb::get, target, fixture.host());
		CHECK(escaped.status == 400);
		CHECK(escaped.body.find("outside the root") == std::string::npos);
	}
}

TEST_CASE("The control port serves only IP literals, localhost and the allowed host names", "[control-port]") {
	Fixture fixture({"ear.lan"});
	const std::string port = ":" + std::to_string(fixture.number());
	for (const std::string& host : std::vector<std::string>{"127.0.0.1" + port, "[::1]" + port, "localhost" + port, "LocalHost" + port,
	                               "ear.lan" + port, "EAR.LAN", "127.0.0.1"}) {
		INFO(host);
		CHECK(httpRequest(fixture.number(), http::verb::get, "/", host).status == 200);
		WsClient client(fixture.number(), WsOptions().host(host));
		CHECK(call(client, 7, "graph.get")["id"] == 7);
	}

	const std::string attacker = "attacker.example" + port;
	const std::vector<std::pair<std::string, std::string>> origin{{"Origin", "http://" + attacker}};
	CHECK(httpRequest(fixture.number(), http::verb::get, "/", attacker, origin).status == 403);
	CHECK(httpRequest(fixture.number(), http::verb::get, "/", "").status == 403);
	CHECK(httpRequest(fixture.number(), http::verb::get, "/", "ear.lan.attacker.example" + port).status == 403);
	try {
		WsClient client(fixture.number(), WsOptions().host(attacker).origin("http://" + attacker));
		FAIL("upgraded with a foreign Host");
	} catch (const Refused& refused) {
		CHECK(refused.status == 403);
	}
}

TEST_CASE("/rpc upgrades without an Origin or with the same host's, never another site's", "[control-port]") {
	Fixture fixture;
	{
		WsClient client(fixture.number());
		CHECK(call(client, 1, "graph.get").contains("result"));
	}
	{
		WsClient client(fixture.number(), WsOptions().origin("http://" + fixture.host()));
		CHECK(call(client, 1, "graph.get").contains("result"));
	}
	for (const std::string& origin : std::vector<std::string>{"http://evil.example", "http://evil.example:" + std::to_string(fixture.number()),
	                                 "null"}) {
		INFO(origin);
		try {
			WsClient client(fixture.number(), WsOptions().origin(origin));
			FAIL("upgraded with a foreign Origin");
		} catch (const Refused& refused) {
			CHECK(refused.status == 403);
		}
	}
}

TEST_CASE("graph.get answers through the gateway and every subscribed client gets every event", "[control-port]") {
	Fixture fixture;
	WsClient first(fixture.number());
	WsClient second(fixture.number());
	const json reply = call(first, 3, "graph.get");
	CHECK(reply["id"] == 3);
	CHECK(reply["result"]["types"] == json{"white"});

	subscribe(first);
	subscribe(second);
	fixture.gateway.emit(removed("a"));
	fixture.gateway.emit(removed("b"));
	for (WsClient* client : {&first, &second}) {
		CHECK(json::parse(client->read()) == removedNotification("a"));
		CHECK(json::parse(client->read()) == removedNotification("b"));
	}
}

TEST_CASE("A client that never reads is closed with 1008 and the others keep every event", "[control-port]") {
	Fixture fixture;
	WsClient reader(fixture.number());
	WsClient stuck(fixture.number(), WsOptions().receiveBuffer(smallWindow));
	subscribe(reader);
	subscribe(stuck);

	// Enough to fill the stuck client's socket buffers and then its queue of 256. Each event is
	// sent only after the reader got the one before, so the hub has offered both clients each.
	constexpr int events = 600;
	for (int i = 0; i < events; ++i) {
		fixture.gateway.emit(ear::control::NodeUpdated{"n" + std::to_string(i), bigSettings});
		REQUIRE(json::parse(reader.read())["params"]["name"] == "n" + std::to_string(i));
	}

	int received = 0;
	try {
		for (;;) {
			stuck.read();
			++received;
		}
	} catch (const Closed& closed) {
		CHECK(closed.code == 1008);
	}
	INFO("the stuck client got " << received);
	CHECK(received < events);

	fixture.gateway.emit(removed("after"));
	CHECK(json::parse(reader.read()) == removedNotification("after"));
}

TEST_CASE("A client that resets while the port writes to it leaves the port running", "[control-port]") {
	Fixture fixture;
	WsClient reader(fixture.number());
	subscribe(reader);
	{
		WsClient resetting(fixture.number(), WsOptions().receiveBuffer(smallWindow));
		subscribe(resetting);
		for (int i = 0; i < 200; ++i) {   // more than its socket buffers hold: a write is waiting
			fixture.gateway.emit(ear::control::NodeUpdated{"n", bigSettings});
			REQUIRE(json::parse(reader.read())["method"] == "node.updated");
		}
		resetting.reset();
	}
	// Writes after the reset (each fails with EPIPE or ECONNRESET, never SIGPIPE).
	for (int i = 0; i < 50; ++i) {
		fixture.gateway.emit(removed("n" + std::to_string(i)));
		REQUIRE(json::parse(reader.read()) == removedNotification("n" + std::to_string(i)));
	}
	WsClient later(fixture.number());
	CHECK(call(later, 9, "graph.get")["id"] == 9);
}

TEST_CASE("stop() closes every client with 1001 while a request is on the main loop", "[control-port]") {
	Fixture fixture;
	WsClient idle(fixture.number());
	WsClient waiting(fixture.number());
	subscribe(idle);

	fixture.gateway.hold = true;
	waiting.send(json{{"jsonrpc", "2.0"}, {"id", 5}, {"method", "graph.get"}}.dump());
	fixture.gateway.held.get_future().wait();   // the request is on the main loop

	// stop() returns once every client has answered its close: here on another thread, while
	// each client reads its close frame and answers it, one after the other.
	auto stopped = std::async(std::launch::async, [&] { fixture.port->stop(); });
	const auto closeCode = [](WsClient& client) -> int {
		try {
			client.read();
			FAIL("a message after stop()");
		} catch (const Closed& closed) {
			return closed.code;
		}
		return 0;
	};
	CHECK(closeCode(idle) == 1001);
	CHECK(stopped.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);   // `waiting` has not answered
	CHECK(closeCode(waiting) == 1001);
	REQUIRE(stopped.wait_for(std::chrono::seconds(10)) == std::future_status::ready);   // only fails the test

	// The held reply now reaches the control thread and is dropped (its session has gone).
	fixture.gateway.release.set_value();
	fixture.mainLoop.drain();
	std::promise<void> ran;
	fixture.control.post([&] { ran.set_value(); });
	ran.get_future().wait();

	fixture.control.stop();   // joins
	auto token = std::make_shared<int>(0);
	bool ranAfterStop = false;
	fixture.control.post([token, &ranAfterStop] { ranAfterStop = true; });
	CHECK(token.use_count() == 1);   // destroyed unrun
	CHECK_FALSE(ranAfterStop);
	fixture.port->stop();   // idempotent
}

TEST_CASE("The level bell sends levels subscribers one complete snapshot", "[control-port]") {
	Fixture fixture;
	WsClient client(fixture.number());
	CHECK(call(client, 2, "levels.subscribe")["result"] == json{{"windowsPerSecond", ear::levelWindowsPerSecond}});
	{
		const std::scoped_lock lock(fixture.levels.mutex);
		fixture.levels.levels = {{"noise", 1600, {{"MONO", {0.5F, 0.25F}}}}};
	}
	fixture.bell.levelsPublished();
	const json levels = json::parse(client.read());
	CHECK(levels["method"] == "levels");
	CHECK(levels["params"]["nodes"]["noise"]["MONO"] == json{0.5, 0.25});
}
