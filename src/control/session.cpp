// The protocol per connection, and the hub of sessions (docs/control-port.md,
// session.hpp). Control thread only. A session turns one received text into one gateway run()
// and its responses; the hub hands each graph event, each graph file change and each levels
// snapshot, encoded once, to the sessions that subscribed.
//
// Snapshot then events: replies, file changes and events arrive in one stream in main-loop order
// (gateway.hpp), so a session marks itself subscribed when its snapshot's reply arrives: every
// change before it is in the snapshot, every change after it is sent. A batch's graph.subscribe and
// graphFile.subscribe take their snapshots after the batch's other commands, in request order
// (JSON-RPC lets a server run a batch in any order), so the changes those commands cause, which
// arrive before the reply, are in the snapshots too.
#include "ear/control/session.hpp"

#include <deque>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ear/control/gestures_wire.hpp"
#include "ear/control/graph_json.hpp"
#include "ear/control/rpc.hpp"

namespace ear::control {
namespace {

using nlohmann::json;

struct Hub;

/// One session's state, shared with its replies in flight (weakly) and its hub. A reply whose
/// session has gone finds it expired and is dropped.
struct SessionState {
	Hub* hub;
	Outbox* out;            // null once the session is destroyed
	SessionToken token = 0; // its gestures' owner (docs/control-port.md#gestures)
	bool graph = false;     // graph.subscribe answered
	bool graphFile = false; // graphFile.subscribe answered
	bool levels = false;
	/// The messages read and not yet answered, the one running first (docs/control-port.md#per-client-limits-and-stopping).
	std::deque<std::string> fifo;
	bool running = false;
	/// The transport's go-ahead to read the next message, held while the FIFO is full.
	std::move_only_function<void()> resume;
};

struct Hub {
	GraphGateway& graph;
	LevelReader& levels;
	TypeSchemas types;                                   // made at boot (docs/control-port.md#type-schemas)
	std::vector<std::shared_ptr<SessionState>> sessions; // attach order
	SessionToken nextToken = 1;

	/// A copy: a send may close a connection and so detach its session.
	std::vector<std::shared_ptr<SessionState>> attached() const { return sessions; }
};

/// How a session finds the hub it is given: the header declares no access between the two
/// classes, so each hub registers itself here for its thread (control thread only, so a session
/// made on another thread finds no hub and throws).
thread_local std::unordered_map<const ControlHub*, Hub*> hubs;

Message share(std::string text) { return std::make_shared<const std::string>(std::move(text)); }

/// Out of its hub's list (below).
void detach(const std::shared_ptr<SessionState>& state);

/// One entry of a received message, in order: what its response will be.
struct Slot {
	enum class Kind { fixed, command, subscribe, fileSubscribe, levels } kind;
	std::optional<RpcId> id; // none: a notification, no response
	std::string text;        // Kind::fixed: the encoded error
	std::size_t command = 0; // Kind::command and the subscribes: index into the results
};

std::optional<std::string> respond(const Slot& slot, const std::vector<CommandResult>& results) {
	if (slot.kind == Slot::Kind::fixed) return slot.text;
	if (!slot.id) return std::nullopt;
	if (slot.kind == Slot::Kind::levels) return encodeResult(*slot.id, {{"windowsPerSecond", levelWindowsPerSecond}});
	auto encoded = encodeCommandResult(results[slot.command]);
	if (!encoded) return encodeError(*slot.id, encoded.error());
	return encodeResult(*slot.id, *encoded);
}

/// The responses to one message, queued in request order, then its subscriptions, then `done`.
void answer(SessionState& state, bool batch, const std::vector<Slot>& slots, const std::vector<CommandResult>& results,
            std::move_only_function<void()>& done) {
	if (state.out == nullptr) { // closed: its replies dropped
		done();
		return;
	}
	std::vector<std::string> responses;
	for (const Slot& slot : slots)
		if (auto response = respond(slot, results)) responses.push_back(std::move(*response));
	if (batch) {
		if (std::string all = encodeBatch(responses); !all.empty()) state.out->send(share(std::move(all)));
	} else {
		for (std::string& response : responses) state.out->send(share(std::move(response)));
	}
	for (const Slot& slot : slots) {
		if (slot.kind == Slot::Kind::subscribe) state.graph = true;
		if (slot.kind == Slot::Kind::fileSubscribe) state.graphFile = true;
		if (slot.kind == Slot::Kind::levels) state.levels = true;
	}
	done();
}

} // namespace

struct ControlSession::Impl {
	std::shared_ptr<SessionState> state;
};

ControlSession::ControlSession(ControlHub& hub, Outbox& out) : impl_(std::make_unique<Impl>()) {
	const auto found = hubs.find(&hub);
	if (found == hubs.end()) throw std::logic_error("ControlSession: its hub is not on this thread");
	impl_->state = std::make_shared<SessionState>(SessionState{found->second, &out, found->second->nextToken++});
	found->second->sessions.push_back(impl_->state);
}

ControlSession::~ControlSession() {
	SessionState& state = *impl_->state;
	state.out = nullptr;
	state.resume = nullptr;
	// Left attached while its FIFO runs on, so the hub's end still reaches it; detached once that drained.
	if (state.fifo.empty()) detach(impl_->state);
}

namespace {

void pump(const std::shared_ptr<SessionState>& state);

/// Out of its hub's list: told nothing more. Its gestures are released only now, once what it queued has run
/// (docs/control-port.md#gestures).
void detach(const std::shared_ptr<SessionState>& state) {
	if (state->hub != nullptr) {
		std::erase(state->hub->sessions, state);
		state->hub->graph.sessionClosed(state->token);
	}
	state->hub = nullptr;
}

void sendIfOpen(SessionState& state, std::string text) {
	if (state.out != nullptr) state.out->send(share(std::move(text)));
}

/// -32000 with data.code: a gesture method's own refusals before any gesture runs.
RpcError gestureError(std::string_view code, std::string message) {
	return RpcError{commandRefused, std::move(message), {{"code", std::string(code)}}};
}

/// A gesture request's answer to `id`, when it has one.
void answerGesture(SessionState& state, const std::optional<RpcId>& id, const GestureAnswer& answer) {
	if (!id) return;
	auto encoded = encodeGestureAnswer(answer);
	sendIfOpen(state, encoded ? encodeResult(*id, *encoded) : encodeError(*id, encoded.error()));
}

/// The gesture id of an end or a cancel, which marks it ending the moment it is read (docs/control-port.md#gestures).
std::optional<std::string> endingOf(const GestureCommand& command) {
	if (const auto* end = std::get_if<EndGesture>(&command)) return end->gesture;
	if (const auto* cancel = std::get_if<CancelGesture>(&command)) return cancel->gesture;
	return std::nullopt;
}

/// One message from the FIFO, whole: its commands to the gateway in one run(); `done` once every response to it is
/// queued (or dropped, the session having closed).
void run(const std::shared_ptr<SessionState>& state, std::string text, std::move_only_function<void()> done) {
	if (state->hub == nullptr) return; // the hub has gone: the port is stopping
	RpcMessage message = parseMessage(text);
	if (!message.batch && message.entries.size() == 1)
		if (auto* request = std::get_if<RpcRequest>(&message.entries.front());
		    request != nullptr && isGestureMethod(request->method)) {
			// An end, or a cancel sent with an id: in order with the session's other requests.
			auto command = decodeGesture(request->method, request->params);
			if (!command) {
				if (request->id) sendIfOpen(*state, encodeError(*request->id, command.error()));
				done();
				return;
			}
			state->hub->graph.gesture(std::move(*command), state->token,
			                          [state, id = request->id, done = std::move(done)](GestureAnswer answer) mutable {
										  answerGesture(*state, id, answer);
										  done();
									  });
			return;
		}

	std::vector<Slot> slots;
	std::vector<Command> commands;
	for (auto& entry : message.entries) {
		if (auto* error = std::get_if<std::string>(&entry)) {
			slots.push_back({Slot::Kind::fixed, std::nullopt, std::move(*error)});
			continue;
		}
		auto& request = std::get<RpcRequest>(entry);
		auto refuse = [&](const RpcError& error) {
			if (request.id) slots.push_back({Slot::Kind::fixed, request.id, encodeError(*request.id, error)});
		};
		if (isGestureMethod(request.method)) { // a batch runs whole as keeper steps; a begin and a tick are not
			refuse(gestureError(notInBatchCode, "not-in-batch " + request.method));
			continue;
		}
		if (request.method == "types.get") { // answered here from the encoding made at boot, without the main loop
			auto ifNoneMatch = decodeGetTypes(request.params);
			if (!ifNoneMatch) refuse(ifNoneMatch.error());
			else if (request.id)
				slots.push_back({Slot::Kind::fixed, request.id,
				                 encodeResult(*request.id, typesResult(state->hub->types, *ifNoneMatch))});
			continue;
		}
		if (request.method == "levels.subscribe") { // answered here, without the main loop
			if (!request.params.is_null())
				if (auto checked = requireObject(request.params, JsonPointer{}, {}); !checked) {
					refuse({invalidParams, checked.error().detail, {{"pointer", checked.error().pointer}}});
					continue;
				}
			slots.push_back({Slot::Kind::levels, request.id, {}});
			continue;
		}
		auto command = decodeCommand(request.method, request.params);
		if (!command) {
			refuse(command.error());
		} else if (request.method == "graph.subscribe") {
			slots.push_back({Slot::Kind::subscribe, request.id, {}});
		} else if (request.method == "graphFile.subscribe") {
			slots.push_back({Slot::Kind::fileSubscribe, request.id, {}});
		} else {
			slots.push_back({Slot::Kind::command, request.id, {}, commands.size()});
			commands.push_back(std::move(*command));
		}
	}
	// The snapshots last, in request order, so they hold every change the batch made (see the top).
	for (Slot& slot : slots) {
		if (slot.kind != Slot::Kind::subscribe && slot.kind != Slot::Kind::fileSubscribe) continue;
		slot.command = commands.size();
		if (slot.kind == Slot::Kind::subscribe) commands.push_back(GetGraph{});
		else commands.push_back(GetGraphFile{});
	}

	if (commands.empty()) {
		answer(*state, message.batch, slots, {}, done);
		return;
	}
	// The state is kept, not watched: a session closed meanwhile still runs the rest of its FIFO in order, its replies
	// dropped (docs/control-port.md#per-client-limits-and-stopping).
	state->hub->graph.run(std::move(commands), [state, batch = message.batch, slots = std::move(slots),
	                                            done = std::move(done)](std::vector<CommandResult> results) mutable {
		if (state->out == nullptr || state->hub == nullptr) {
			done();
			return;
		}
		for (CommandResult& result : results)
			if (auto* view = std::get_if<GraphView>(&result)) view->typeSchemaHash = state->hub->types.hash;
		answer(*state, batch, slots, results, done);
	});
}

/// The FIFO's front runs when none is running; once answered it leaves the FIFO, a held go-ahead is given, and the
/// next runs.
void pump(const std::shared_ptr<SessionState>& state) {
	if (state->running || state->fifo.empty() || state->hub == nullptr) return;
	state->running = true;
	std::string text = std::move(state->fifo.front());
	run(state, std::move(text), [state] {
		state->running = false;
		state->fifo.pop_front();
		if (state->out == nullptr && state->fifo.empty()) { // closed, and the last of its FIFO has run
			detach(state);
			return;
		}
		if (state->resume && state->fifo.size() < sessionFifoBound) std::exchange(state->resume, nullptr)();
		pump(state);
	});
}

/// A gesture.begin, a gesture.change and a gesture.cancel sent as a notification are taken the moment they are read,
/// ahead of the FIFO; an end or a cancel read marks its gesture ending at once, then waits its turn
/// (docs/control-port.md#gestures). True when `text` was taken here.
bool skipsFifo(const std::shared_ptr<SessionState>& state, const std::string& text) {
	if (text.find("gesture.") == std::string::npos) return false; // no gesture method: the common case, unparsed
	RpcMessage message = parseMessage(text);
	if (message.batch || message.entries.size() != 1) return false;
	auto* request = std::get_if<RpcRequest>(&message.entries.front());
	if (request == nullptr || !isGestureMethod(request->method)) return false;
	const bool tick = request->method == "gesture.change";
	if (tick && request->id) { // a tick must never wait for a reply
		sendIfOpen(*state, encodeError(*request->id, RpcError{invalidRequest,
		                                                      "gesture.change is a notification",
		                                                      {{"code", std::string(notificationOnlyCode)}}}));
		return true;
	}
	const bool queued = request->method == "gesture.end" || (request->method == "gesture.cancel" && request->id);
	auto command = decodeGesture(request->method, request->params);
	if (!command) {
		if (queued) return false; // refused in its turn
		if (request->id) sendIfOpen(*state, encodeError(*request->id, command.error()));
		return true;
	}
	if (const auto ending = endingOf(*command)) state->hub->graph.ending(*ending);
	if (queued) return false;
	std::move_only_function<void(GestureAnswer)> reply;
	if (request->id) reply = [state, id = request->id](GestureAnswer answer) { answerGesture(*state, id, answer); };
	state->hub->graph.gesture(std::move(*command), state->token, std::move(reply));
	return true;
}

} // namespace

void ControlSession::received(std::string text, std::move_only_function<void()> done) {
	const std::shared_ptr<SessionState>& state = impl_->state;
	if (state->hub == nullptr) return; // the hub has gone: the port is stopping
	if (skipsFifo(state, text)) {
		done();
		return;
	}
	state->fifo.push_back(std::move(text));
	if (state->fifo.size() < sessionFifoBound) done();
	else state->resume = std::move(done);
	pump(state);
}

struct ControlHub::Impl {
	Hub hub;
};

ControlHub::ControlHub(GraphGateway& graph, LevelReader& levels, TypeSchemas types)
	: impl_(std::make_unique<Impl>(Impl{Hub{graph, levels, std::move(types), {}}})) {
	hubs.emplace(this, &impl_->hub);
	graph.watch(*this);
}

ControlHub::~ControlHub() {
	hubs.erase(this);
	for (const auto& session : impl_->hub.sessions) session->hub = nullptr;
}

void ControlHub::graphChanged(GraphEvent event) {
	Message message;
	for (const auto& session : impl_->hub.attached()) {
		if (session->out == nullptr || !session->graph) continue;
		if (!message) message = share(encodeEvent(event));
		session->out->send(message);
	}
}

void ControlHub::graphFileChanged(GraphFileChanged change) {
	Message message;
	for (const auto& session : impl_->hub.attached()) {
		if (session->out == nullptr || !session->graphFile) continue;
		if (!message) message = share(encodeGraphFileChanged(change));
		session->out->send(message);
	}
}

void ControlHub::gestureChanged(GestureNote note) {
	// Addressed (docs/control-port.md#gesturechanged): begin and change to every graph-subscribed session but the
	// owner; end and aborted to those and to the owner and the session that ended it, whatever their subscriptions;
	// tick-refused to the owner alone.
	Message message;
	for (const auto& session : impl_->hub.attached()) {
		if (session->out == nullptr) continue;
		const bool owner = session->token == note.owner;
		const bool by = note.by && session->token == *note.by;
		bool to = false;
		switch (note.phase) {
		case GesturePhase::begin:
		case GesturePhase::change:
			to = session->graph && !owner;
			break;
		case GesturePhase::end:
		case GesturePhase::aborted:
			to = session->graph || owner || by;
			break;
		case GesturePhase::tickRefused:
			to = owner;
			break;
		}
		if (!to) continue;
		if (!message) message = share(encodeGestureChanged(note));
		session->out->send(message);
	}
}

void ControlHub::levelsPublished() {
	Message message;
	for (const auto& session : impl_->hub.attached()) {
		if (session->out == nullptr || !session->levels) continue;
		if (!message) message = share(encodeLevels(impl_->hub.levels.latestLevels()));
		session->out->sendLevels(message);
	}
}

} // namespace ear::control
