// Port 7777: HTTP for the UI's files and WebSocket JSON-RPC at /rpc, on the control thread
// (docs/design/batch2.md sections 2, 4.6 and 6, control_port.hpp). The shape is Beast's
// advanced_server example (HTTP and WebSocket on one port, the upgrade told apart with
// websocket::is_upgrade) with its websocket-chat-multi example's per-connection queue of shared
// messages. Everything below runs on the control thread; start() and stop() hand their work to
// it and wait for the answer.
//
// No timers: a connection stays until its peer or stop() ends it (the owner's "It's all event
// driven"). A peer that stops reading holds its socket until it goes or the control thread
// stops; its queue is bounded, so it holds no more than that. stop() waits for every close
// handshake, so a peer that never answers the close frame (nor goes) holds stop() too.
#include "ear/server/control_port.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <deque>
#include <future>
#include <ranges>
#include <unordered_map>
#include <utility>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <fcntl.h>
#include <unistd.h>

#include "control_context.hpp"
#include "ear/control/session.hpp"
#include "request_rules.hpp"

namespace ear::server {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using boost::system::error_code;

// ---- Request rules (request_rules.hpp) ----

namespace {

bool equalNoCase(std::string_view a, std::string_view b) {
	return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

bool allDigits(std::string_view text) {
	return !text.empty() && text.size() <= 5 && std::ranges::all_of(text, [](unsigned char c) { return std::isdigit(c); });
}

} // namespace

bool hostAllowed(std::string_view host, const std::vector<std::string>& allowedHosts) {
	error_code ec;
	if (host.starts_with('[')) {   // [IPv6]:port
		const auto close = host.find(']');
		if (close == std::string_view::npos) return false;
		const std::string_view rest = host.substr(close + 1);
		if (!rest.empty() && !(rest.starts_with(':') && allDigits(rest.substr(1)))) return false;
		asio::ip::make_address_v6(host.substr(1, close - 1), ec);
		return !ec;
	}
	std::string_view name = host;
	if (const auto colon = host.find(':'); colon != std::string_view::npos) {
		if (!allDigits(host.substr(colon + 1))) return false;
		name = host.substr(0, colon);
	}
	if (name.empty()) return false;
	asio::ip::make_address_v4(name, ec);
	if (!ec || equalNoCase(name, "localhost")) return true;
	return std::ranges::any_of(allowedHosts, [&](const std::string& allowed) { return equalNoCase(name, allowed); });
}

bool originAllowed(std::optional<std::string_view> origin, std::string_view host) {
	if (!origin) return true;
	const auto scheme = origin->find("://");
	if (scheme == std::string_view::npos) return false;   // "null" and the like
	std::string_view authority = origin->substr(scheme + 3);
	authority = authority.substr(0, authority.find('/'));
	return !host.empty() && equalNoCase(authority, host);
}

std::optional<std::filesystem::path> fileUnder(const std::filesystem::path& root, std::string_view target) {
	target = target.substr(0, target.find_first_of("?#"));
	if (!target.starts_with('/')) return std::nullopt;
	std::filesystem::path file = root;
	for (const auto part : std::views::split(target.substr(1), '/')) {
		const std::string_view segment(part.begin(), part.end());
		if (segment == "..") return std::nullopt;
		if (!segment.empty() && segment != ".") file /= segment;   // never an absolute part
	}
	if (target.ends_with('/')) file /= "index.html";
	return file;
}

std::string_view contentType(const std::filesystem::path& file) {
	static const std::unordered_map<std::string_view, std::string_view> types{
		{".html", "text/html"},       {".htm", "text/html"},         {".js", "text/javascript"},
		{".mjs", "text/javascript"},  {".css", "text/css"},          {".json", "application/json"},
		{".map", "application/json"}, {".svg", "image/svg+xml"},     {".png", "image/png"},
		{".ico", "image/x-icon"},     {".woff2", "font/woff2"},      {".wasm", "application/wasm"},
		{".txt", "text/plain"}};
	const std::string extension = file.extension().string();
	const auto found = std::ranges::find_if(types, [&](const auto& entry) { return equalNoCase(entry.first, extension); });
	return found == types.end() ? "application/octet-stream" : found->second;
}

// ---- The port ----

namespace {

/// Largest WebSocket message and HTTP request body read (control_port.hpp).
constexpr std::size_t maxMessageBytes = 64 * 1024;
/// Responses and notifications a connection may have waiting for its socket (section 6); one
/// more closes it with 1008.
constexpr std::size_t maxQueuedMessages = 256;

class WsSession;
class HttpSession;

/// stop()'s answer: the promise is kept when the last copy goes, which is once every
/// connection's close handshake has finished (or failed) and its handler has run, or the
/// handler was destroyed unrun with the control thread. Event-driven: no deadline.
class Closing {
public:
	explicit Closing(std::promise<void> done) : done_(std::move(done)) {}
	Closing(const Closing&) = delete;
	Closing& operator=(const Closing&) = delete;
	~Closing() { done_.set_value(); }

private:
	std::promise<void> done_;
};

/// The port's state, shared with every connection: a connection's handlers may outlive the
/// ControlPort (a peer that never reads), and they then find it stopped.
struct Shared {
	ControlPortSettings settings;
	tcp::acceptor acceptor;
	asio::posix::stream_descriptor bell;
	std::unique_ptr<control::ControlHub> hub;   // null once stopped
	std::uint16_t port = 0;                     // the bound port
	bool stopped = false;
	/// Open WebSocket connections, held here while open (a connection waiting for its reply has
	/// no Asio operation holding it).
	std::unordered_map<WsSession*, std::shared_ptr<WsSession>> sockets;
	/// HTTP connections, held by their own operations.
	std::unordered_map<HttpSession*, std::weak_ptr<HttpSession>> requests;

	/// A descriptor kept in reserve for a full descriptor table (acceptNext).
	int spare = openSpare();

	Shared(ControlPortSettings s, asio::io_context& io) : settings(std::move(s)), acceptor(io), bell(io) {}
	~Shared() {
		if (bell.is_open()) bell.release();   // the LevelBell owns its descriptor
		if (spare >= 0) ::close(spare);
	}
	Shared(const Shared&) = delete;
	Shared& operator=(const Shared&) = delete;

	static int openSpare() noexcept { return ::open("/dev/null", O_RDONLY | O_CLOEXEC); }
};

class WsSession final : public control::Outbox, public std::enable_shared_from_this<WsSession> {
public:
	WsSession(std::shared_ptr<Shared> shared, tcp::socket socket) : shared_(std::move(shared)), ws_(std::move(socket)) {}

	void accept(http::request<http::string_body> request) {
		shared_->sockets.emplace(this, shared_from_this());
		ws_.read_message_max(maxMessageBytes);
		ws_.text(true);
		ws_.async_accept(request, [self = shared_from_this()](error_code ec) { self->accepted(ec); });
	}

	/// stop(): close with `code` (1001), or drop a connection still in its handshake. `closing`
	/// is held until the close handshake ends.
	void shutdown(websocket::close_code code, std::shared_ptr<Closing> closing) {
		if (!accepted_) {
			forget();
			error_code ignored;
			beast::get_lowest_layer(ws_).close(ignored);
			return;
		}
		closeWith(code, std::move(closing));
	}

	void send(control::Message message) override {
		if (closing_) return;
		if (queue_.size() >= maxQueuedMessages) {
			overflow();
			return;
		}
		queue_.push_back(std::move(message));
		write();
	}

	void sendLevels(control::Message message) override {
		if (closing_) return;
		levels_ = std::move(message);   // a complete snapshot: it replaces one not yet written
		write();
	}

private:
	void accepted(error_code ec) {
		if (closing_) return;
		if (ec || !shared_->hub) {
			forget();
			return;
		}
		accepted_ = true;
		session_ = std::make_unique<control::ControlSession>(*shared_->hub, *this);
		read();
	}

	void read() {
		ws_.async_read(buffer_, [self = shared_from_this()](error_code ec, std::size_t) { self->received(ec); });
	}

	void received(error_code ec) {
		if (closing_) return;
		if (ec) {   // the peer closed, reset or broke the protocol
			closing_ = true;
			forget();
			return;
		}
		std::string text = beast::buffers_to_string(buffer_.data());
		buffer_.consume(buffer_.size());
		// One message in flight: the next is read once every response to this one is queued.
		session_->received(std::move(text), [weak = weak_from_this()] {
			if (const auto self = weak.lock(); self && !self->closing_) self->read();
		});
	}

	void write() {
		if (writing_ || closing_) return;
		if (!queue_.empty()) {
			writing_ = std::move(queue_.front());
			queue_.pop_front();
		} else if (levels_) {
			writing_ = std::move(levels_);
		} else {
			return;
		}
		ws_.async_write(asio::buffer(*writing_), [self = shared_from_this()](error_code ec, std::size_t) {
			self->writing_.reset();
			if (ec) {
				self->closing_ = true;
				self->forget();
				return;
			}
			self->write();
		});
	}

	/// A full queue: this connection is closed with 1008 and the client subscribes again. send()
	/// may be inside the session's own answer, so the session is let go in a later turn.
	void overflow() {
		closing_ = true;
		queue_.clear();
		levels_.reset();
		asio::post(ws_.get_executor(), [self = shared_from_this()] { self->closeWith(websocket::close_code::policy_error); });
	}

	/// Beast's async_close writes the close frame (after a write in progress), reads until the
	/// peer's close frame, discarding what the peer sent meanwhile, and only then closes the
	/// socket; its handler runs after that. A socket closed with the peer's data unread would
	/// reset the connection instead (the peer would never see 1001).
	void closeWith(websocket::close_code code, std::shared_ptr<Closing> closing = nullptr) {
		closing_ = true;
		queue_.clear();
		levels_.reset();
		forget();
		if (closeSent_) return;
		closeSent_ = true;
		ws_.async_close(code, [self = shared_from_this(), closing = std::move(closing)](error_code) {});
	}

	/// Detach from the hub and the port. Never inside the session's own calls.
	void forget() {
		session_.reset();
		shared_->sockets.erase(this);
	}

	std::shared_ptr<Shared> shared_;
	websocket::stream<tcp::socket> ws_;
	beast::flat_buffer buffer_;
	std::unique_ptr<control::ControlSession> session_;
	std::deque<control::Message> queue_;
	control::Message levels_;    // the newest levels snapshot not yet written
	control::Message writing_;   // the message being written (kept alive for its buffer)
	bool accepted_ = false;
	bool closing_ = false;
	bool closeSent_ = false;
};

http::message_generator plain(const http::request<http::string_body>& request, http::status status,
                              std::string_view text) {
	http::response<http::string_body> response{status, request.version()};
	response.set(http::field::content_type, "text/plain");
	if (status == http::status::method_not_allowed) response.set(http::field::allow, "GET, HEAD");
	response.keep_alive(request.keep_alive());
	response.body() = std::string(text) + "\n";
	response.prepare_payload();
	return response;
}

http::message_generator serveFile(const http::request<http::string_body>& request, const std::filesystem::path& root) {
	if (request.method() != http::verb::get && request.method() != http::verb::head)
		return plain(request, http::status::method_not_allowed, "method not allowed");
	const auto file = fileUnder(root, request.target());
	if (!file) return plain(request, http::status::bad_request, "bad request target");
	std::error_code fileError;
	if (!std::filesystem::is_regular_file(*file, fileError)) return plain(request, http::status::not_found, "not found");
	http::file_body::value_type body;
	error_code ec;
	body.open(file->c_str(), beast::file_mode::scan, ec);
	if (ec) return plain(request, http::status::not_found, "not found");
	const auto size = body.size();
	const std::string type(contentType(*file));
	if (request.method() == http::verb::head) {
		http::response<http::empty_body> response{http::status::ok, request.version()};
		response.set(http::field::content_type, type);
		response.content_length(size);
		response.keep_alive(request.keep_alive());
		return response;
	}
	http::response<http::file_body> response{std::piecewise_construct, std::make_tuple(std::move(body)),
	                                          std::make_tuple(http::status::ok, request.version())};
	response.set(http::field::content_type, type);
	response.content_length(size);
	response.keep_alive(request.keep_alive());
	return response;
}

class HttpSession final : public std::enable_shared_from_this<HttpSession> {
public:
	HttpSession(std::shared_ptr<Shared> shared, tcp::socket socket)
		: shared_(std::move(shared)), socket_(std::move(socket)) {}
	~HttpSession() { shared_->requests.erase(this); }

	void start() {
		shared_->requests.emplace(this, weak_from_this());
		read();
	}

	void close() {
		error_code ignored;
		socket_.close(ignored);
	}

private:
	void read() {
		parser_.emplace();
		parser_->body_limit(maxMessageBytes);
		http::async_read(socket_, buffer_, *parser_,
		                 [self = shared_from_this()](error_code ec, std::size_t) { self->received(ec); });
	}

	void received(error_code ec) {
		if (ec || shared_->stopped) {
			close();
			return;
		}
		http::request<http::string_body> request = parser_->release();
		const std::string_view host = request[http::field::host];
		if (!hostAllowed(host, shared_->settings.allowedHosts))
			return respond(plain(request, http::status::forbidden, "forbidden host"));
		if (websocket::is_upgrade(request)) {
			const std::string_view target = request.target();
			if (target.substr(0, target.find_first_of("?#")) != "/rpc")
				return respond(plain(request, http::status::not_found, "not found"));
			std::optional<std::string_view> origin;
			if (const auto found = request.find(http::field::origin); found != request.end()) origin = found->value();
			if (!originAllowed(origin, host)) return respond(plain(request, http::status::forbidden, "forbidden origin"));
			std::make_shared<WsSession>(shared_, std::move(socket_))->accept(std::move(request));
			return;
		}
		respond(serveFile(request, shared_->settings.webRoot));
	}

	void respond(http::message_generator message) {
		const bool keepAlive = message.keep_alive();
		beast::async_write(socket_, std::move(message), [self = shared_from_this(), keepAlive](error_code ec, std::size_t) {
			if (ec || !keepAlive || self->shared_->stopped) {
				error_code ignored;
				self->socket_.shutdown(tcp::socket::shutdown_send, ignored);
				return;
			}
			self->read();
		});
	}

	std::shared_ptr<Shared> shared_;
	tcp::socket socket_;
	beast::flat_buffer buffer_;
	std::optional<http::request_parser<http::string_body>> parser_;
};

void acceptNext(const std::shared_ptr<Shared>& shared);

/// With the descriptor table full (EMFILE, ENFILE) a waiting connection stays queued and the
/// acceptor stays readable, so accepting again at once would spin. The usual remedy (libuv,
/// nginx): free the spare descriptor, accept the waiting connection (if any) and close it, take
/// the spare back, then accept again only once the acceptor is readable: the next connection is
/// the next event. The refused peer sees its connection closed.
void refuseOne(const std::shared_ptr<Shared>& shared) {
	if (shared->spare >= 0) ::close(shared->spare);
	error_code ignored;
	shared->acceptor.non_blocking(true, ignored);   // an empty queue answers would_block
	tcp::socket refused(shared->acceptor.get_executor());
	shared->acceptor.accept(refused, ignored);
	refused.close(ignored);
	shared->spare = Shared::openSpare();
	shared->acceptor.async_wait(tcp::acceptor::wait_read, [shared](error_code ec) {
		if (shared->stopped || ec == asio::error::operation_aborted) return;
		acceptNext(shared);
	});
}

void acceptNext(const std::shared_ptr<Shared>& shared) {
	shared->acceptor.async_accept([shared](error_code ec, tcp::socket socket) {
		if (shared->stopped || ec == asio::error::operation_aborted) return;
		if (ec == asio::error::no_descriptors || ec.value() == ENFILE) {
			refuseOne(shared);
			return;
		}
		if (!ec) {
			// Messages are small and each is wanted now (levels are 30 a second): no Nagle wait
			// behind the peer's delayed acknowledgement (tens of ms per message on loopback).
			error_code ignored;
			socket.set_option(tcp::no_delay(true), ignored);
			std::make_shared<HttpSession>(shared, std::move(socket))->start();
		}
		acceptNext(shared);
	});
}

/// The bell rings at most once until read; it is read before the levels, so a window published
/// meanwhile rings it again.
void awaitBell(const std::shared_ptr<Shared>& shared) {
	shared->bell.async_wait(asio::posix::descriptor_base::wait_read, [shared](error_code ec) {
		if (ec || shared->stopped) return;
		std::uint64_t rings = 0;
		[[maybe_unused]] const auto got = ::read(shared->bell.native_handle(), &rings, sizeof rings);
		if (shared->hub) shared->hub->levelsPublished();
		awaitBell(shared);
	});
}

using Opened = std::expected<std::shared_ptr<Shared>, ControlPortError>;

/// On the control thread.
Opened open(const ControlPortSettings& settings, control::GraphGateway& graph, LevelReader& levels, LevelBell& bell) {
	auto shared = std::make_shared<Shared>(settings, currentControlContext());
	const std::string where = settings.address + ":" + std::to_string(settings.port);
	error_code ec;
	const auto address = asio::ip::make_address(settings.address, ec);
	if (ec) return std::unexpected(ControlPortError{"control address " + settings.address + ": " + ec.message()});
	const tcp::endpoint endpoint{address, settings.port};
	if (shared->acceptor.open(endpoint.protocol(), ec); !ec)
		if (shared->acceptor.set_option(asio::socket_base::reuse_address(true), ec); !ec)
			if (shared->acceptor.bind(endpoint, ec); !ec) shared->acceptor.listen(asio::socket_base::max_listen_connections, ec);
	if (ec) return std::unexpected(ControlPortError{"cannot listen on " + where + ": " + ec.message()});
	shared->port = shared->acceptor.local_endpoint().port();
	if (shared->bell.assign(bell.fd(), ec); ec)
		return std::unexpected(ControlPortError{"level bell: " + ec.message()});
	shared->hub = std::make_unique<control::ControlHub>(graph, levels);
	acceptNext(shared);
	awaitBell(shared);
	return shared;
}

/// On the control thread: the listener, every connection (1001) and the hub. `closing` is
/// released once every connection's close handshake has ended.
void close(Shared& shared, const std::shared_ptr<Closing>& closing) {
	shared.stopped = true;
	error_code ignored;
	shared.acceptor.close(ignored);
	shared.bell.cancel(ignored);
	shared.bell.release();
	std::vector<std::shared_ptr<WsSession>> sockets;   // a copy: each shutdown leaves the map
	for (const auto& [key, socket] : shared.sockets) sockets.push_back(socket);
	for (const auto& socket : sockets) socket->shutdown(websocket::close_code::going_away, closing);
	std::vector<std::shared_ptr<HttpSession>> requests;
	for (const auto& [key, request] : shared.requests)
		if (auto http = request.lock()) requests.push_back(std::move(http));
	for (const auto& http : requests) http->close();
	shared.hub.reset();
}

} // namespace

struct ControlPort::Impl {
	ControlThread& thread;
	std::shared_ptr<Shared> shared;
	std::uint16_t port;
	std::atomic<bool> stopped = false;
};

std::expected<std::unique_ptr<ControlPort>, ControlPortError> ControlPort::start(const ControlPortSettings& settings,
                                                                                ControlThread& thread,
                                                                                control::GraphGateway& graph,
                                                                                LevelReader& levels, LevelBell& bell) {
	// The promise lives only in the work: work the thread destroys unrun breaks it.
	std::promise<Opened> promise;
	auto opened = promise.get_future();
	thread.post([promise = std::move(promise), &settings, &graph, &levels, &bell]() mutable {
		try {
			promise.set_value(open(settings, graph, levels, bell));
		} catch (...) {
			promise.set_exception(std::current_exception());
		}
	});
	Opened result = [&]() -> Opened {
		try {
			return opened.get();
		} catch (const std::future_error&) {   // the work was destroyed unrun
			return std::unexpected(ControlPortError{"the control thread has stopped"});
		}
	}();
	if (!result) return std::unexpected(result.error());
	const std::uint16_t port = (*result)->port;
	return std::unique_ptr<ControlPort>(
		new ControlPort(std::make_unique<Impl>(thread, std::move(*result), port)));
}

ControlPort::ControlPort(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ControlPort::~ControlPort() { stop(); }

std::uint16_t ControlPort::port() const noexcept { return impl_->port; }

void ControlPort::stop() {
	if (impl_->stopped.exchange(true)) return;
	std::promise<void> promise;   // as in start(): only the work holds it
	auto closed = promise.get_future();
	impl_->thread.post([shared = impl_->shared, promise = std::move(promise)]() mutable {
		close(*shared, std::make_shared<Closing>(std::move(promise)));
	});
	try {
		closed.get();
	} catch (const std::future_error&) {
		// The control thread stopped first: its work, and with it every connection, is gone.
	}
}

} // namespace ear::server
