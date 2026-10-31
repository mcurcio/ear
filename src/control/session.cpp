// The protocol per connection, and the hub of sessions (docs/design/batch2.md sections 4.5 and 6,
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
	Outbox* out;   // null once the session is destroyed
	bool graph = false;       // graph.subscribe answered
	bool graphFile = false;   // graphFile.subscribe answered
	bool levels = false;
};

struct Hub {
	GraphGateway& graph;
	LevelReader& levels;
	std::vector<std::shared_ptr<SessionState>> sessions;   // attach order

	/// A copy: a send may close a connection and so detach its session.
	std::vector<std::shared_ptr<SessionState>> attached() const { return sessions; }
};

/// How a session finds the hub it is given: the header declares no access between the two
/// classes, so each hub registers itself here for its thread (control thread only, so a session
/// made on another thread finds no hub and throws).
thread_local std::unordered_map<const ControlHub*, Hub*> hubs;

Message share(std::string text) { return std::make_shared<const std::string>(std::move(text)); }

/// One entry of a received message, in order: what its response will be.
struct Slot {
	enum class Kind { fixed, command, subscribe, fileSubscribe, levels } kind;
	std::optional<RpcId> id;   // none: a notification, no response
	std::string text;          // Kind::fixed: the encoded error
	std::size_t command = 0;   // Kind::command and the subscribes: index into the results
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
void answer(SessionState& state, bool batch, const std::vector<Slot>& slots,
            const std::vector<CommandResult>& results, std::move_only_function<void()>& done) {
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
	if (state.hub != nullptr) std::erase(state.hub->sessions, impl_->state);
	state.hub = nullptr;
	state.out = nullptr;
}

void ControlSession::received(std::string text, std::move_only_function<void()> done) {
	const std::shared_ptr<SessionState>& state = impl_->state;
	if (state->hub == nullptr) return;   // the hub has gone: the port is stopping
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
		if (request.method == "levels.subscribe") {   // answered here, without the main loop
			if (!request.params.is_null())
				if (auto checked = requireObject(request.params, JsonPointer{}, {}); !checked) {
					refuse({invalidParams, checked.error().detail, nullptr});
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
	state->hub->graph.run(std::move(commands),
	                      [weak = std::weak_ptr<SessionState>(state), batch = message.batch, slots = std::move(slots),
	                       done = std::move(done)](std::vector<CommandResult> results) mutable {
		                      const auto alive = weak.lock();
		                      if (!alive || alive->out == nullptr) return;   // the session closed meanwhile
		                      answer(*alive, batch, slots, results, done);
	                      });
}

struct ControlHub::Impl {
	Hub hub;
};

ControlHub::ControlHub(GraphGateway& graph, LevelReader& levels)
	: impl_(std::make_unique<Impl>(Impl{Hub{graph, levels, {}}})) {
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
