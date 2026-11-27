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
	impl_->state = std::make_shared<SessionState>(SessionState{found->second, &out});
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

/// Out of its hub's list: told nothing more.
void detach(const std::shared_ptr<SessionState>& state) {
	if (state->hub != nullptr) std::erase(state->hub->sessions, state);
	state->hub = nullptr;
}

/// One message from the FIFO, whole: its commands to the gateway in one run(); `done` once every response to it is
/// queued (or dropped, the session having closed).
void run(const std::shared_ptr<SessionState>& state, std::string text, std::move_only_function<void()> done) {
	if (state->hub == nullptr) return; // the hub has gone: the port is stopping
	RpcMessage message = parseMessage(text);

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

} // namespace

void ControlSession::received(std::string text, std::move_only_function<void()> done) {
	const std::shared_ptr<SessionState>& state = impl_->state;
	if (state->hub == nullptr) return; // the hub has gone: the port is stopping
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

void ControlHub::levelsPublished() {
	Message message;
	for (const auto& session : impl_->hub.attached()) {
		if (session->out == nullptr || !session->levels) continue;
		if (!message) message = share(encodeLevels(impl_->hub.levels.latestLevels()));
		session->out->sendLevels(message);
	}
}

} // namespace ear::control
