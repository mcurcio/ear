// The tests' client of the control port (docs/design/batch2.md sections 4.6 and 6): one HTTP
// request on a fresh connection, and a WebSocket client, both on Boost.Beast. Every call waits
// for its own completion; the deadline only fails the test (it throws), it never paces one.
#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

namespace ear::test {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

/// How long one call may wait before the test fails.
inline constexpr std::chrono::seconds clientDeadline{10};

struct TimedOut : std::runtime_error {
	TimedOut() : std::runtime_error("the control port did not answer before the deadline") {}
};

/// The server closed the WebSocket connection; `code` is its close code (0 when none was sent).
struct Closed : std::runtime_error {
	explicit Closed(std::uint16_t c) : std::runtime_error("closed with " + std::to_string(c)), code(c) {}
	std::uint16_t code;
};

/// A WebSocket handshake the server refused, with its HTTP status.
struct Refused : std::runtime_error {
	explicit Refused(unsigned s) : std::runtime_error("upgrade refused with " + std::to_string(s)), status(s) {}
	unsigned status;
};

namespace detail {

/// Runs one asynchronous operation started by `start(handler)` to completion or the deadline.
template <class Start>
boost::system::error_code complete(net::io_context& io, tcp::socket& socket, Start&& start) {
	bool done = false;
	boost::system::error_code result;
	start([&](boost::system::error_code ec, auto&&...) {
		done = true;
		result = ec;
	});
	io.restart();
	io.run_for(clientDeadline);
	if (!done) {
		boost::system::error_code ignored;
		socket.cancel(ignored);
		io.restart();
		io.run();   // the cancelled operation's handler, which names locals of this frame
		throw TimedOut();
	}
	return result;
}

inline void connect(net::io_context& io, tcp::socket& socket, std::uint16_t port) {
	const tcp::endpoint server{net::ip::make_address("127.0.0.1"), port};
	const auto ec = complete(io, socket, [&](auto handler) { socket.async_connect(server, std::move(handler)); });
	if (ec) throw boost::system::system_error(ec, "connect");
}

} // namespace detail

struct HttpReply {
	unsigned status;
	std::string contentType;
	std::string contentLength;
	std::string body;
};

/// One request to 127.0.0.1:`port` on a new connection. `host` is the Host header (none if
/// empty); `headers` are added as given. The target is sent exactly as written.
inline HttpReply httpRequest(std::uint16_t port, http::verb method, const std::string& target, const std::string& host,
                             const std::vector<std::pair<std::string, std::string>>& headers = {}) {
	net::io_context io;
	tcp::socket socket(io);
	detail::connect(io, socket, port);
	http::request<http::string_body> request{method, target, 11};
	if (!host.empty()) request.set(http::field::host, host);
	for (const auto& [name, value] : headers) request.set(name, value);
	request.prepare_payload();
	auto ec = detail::complete(io, socket, [&](auto handler) { http::async_write(socket, request, std::move(handler)); });
	if (ec) throw boost::system::system_error(ec, "write");
	beast::flat_buffer buffer;
	http::response_parser<http::string_body> parser;
	parser.skip(method == http::verb::head);   // a HEAD response has no body
	ec = detail::complete(io, socket, [&](auto handler) { http::async_read(socket, buffer, parser, std::move(handler)); });
	if (ec) throw boost::system::system_error(ec, "read");
	auto& response = parser.get();
	return {response.result_int(), std::string(response[http::field::content_type]),
	        std::string(response[http::field::content_length]), response.body()};
}

/// How a WsClient connects: WsOptions().host("localhost:7777").origin("http://localhost:7777").
struct WsOptions {
	std::string hostHeader;                    // empty: 127.0.0.1:<port>
	std::optional<std::string> originHeader;   // none: no Origin header
	std::optional<int> receiveBufferBytes;     // SO_RCVBUF, set before connecting
	WsOptions& host(std::string value) { hostHeader = std::move(value); return *this; }
	WsOptions& origin(std::string value) { originHeader = std::move(value); return *this; }
	WsOptions& receiveBuffer(int bytes) { receiveBufferBytes = bytes; return *this; }
};

class WsClient {
public:
	/// Connects and upgrades to /rpc; throws Refused when the server answers with a status.
	explicit WsClient(std::uint16_t port, const WsOptions& options = WsOptions()) : ws_(io_) {
		tcp::socket& socket = ws_.next_layer();
		if (options.receiveBufferBytes) {
			socket.open(tcp::v4());
			socket.set_option(net::socket_base::receive_buffer_size(*options.receiveBufferBytes));
		}
		detail::connect(io_, socket, port);
		if (options.originHeader)
			ws_.set_option(websocket::stream_base::decorator(
				[origin = *options.originHeader](websocket::request_type& request) { request.set(http::field::origin, origin); }));
		const std::string host =
			options.hostHeader.empty() ? "127.0.0.1:" + std::to_string(port) : options.hostHeader;
		websocket::response_type response;
		const auto ec = detail::complete(io_, socket, [&](auto handler) {
			ws_.async_handshake(response, host, "/rpc", std::move(handler));
		});
		if (ec == websocket::error::upgrade_declined) throw Refused(response.result_int());
		if (ec) throw boost::system::system_error(ec, "handshake");
	}

	void send(const std::string& text) {
		const auto ec = detail::complete(io_, ws_.next_layer(),
		                                 [&](auto handler) { ws_.async_write(net::buffer(text), std::move(handler)); });
		if (ec) throw boost::system::system_error(ec, "send");
	}

	/// The next message; throws Closed once the server has closed the connection.
	std::string read() {
		beast::flat_buffer buffer;
		const auto ec = detail::complete(io_, ws_.next_layer(),
		                                 [&](auto handler) { ws_.async_read(buffer, std::move(handler)); });
		if (ec == websocket::error::closed) throw Closed(ws_.reason().code);
		if (ec) throw boost::system::system_error(ec, "read");
		return beast::buffers_to_string(buffer.data());
	}

	/// Ends the connection with a TCP reset (SO_LINGER 0), as a client that crashes would.
	void reset() {
		tcp::socket& socket = ws_.next_layer();
		socket.set_option(net::socket_base::linger(true, 0));
		socket.close();
	}

private:
	net::io_context io_;
	websocket::stream<tcp::socket> ws_;
};

} // namespace ear::test
