// The one crossing between the main loop and the control thread (docs/architecture.md#threads, gateway.hpp;
// docs/architecture.md#how-a-change-moves): the Active Object pattern. Commands go whole to the graph file keeper; its
// reply, every graph file change and every graph event cross back as work posted to the control executor, all from the
// main loop, so the control thread sees one stream in main-loop order.
//
// Each node's health as last sent and when its phase began live here on the main loop
// (docs/control-port.md#node-health): node.state goes out when any member changes, `since` moves only
// with the phase, and a snapshot's `since` is filled from the same map when the graph is read (stamp, called by the
// keeper's main-loop side at the read), so it is the one the snapshot's phase had.
//
// Posted work captures what it needs by value and the executors by reference (they outlive the
// gateway), never the gateway itself, except watch(), which runs before any command.
#include "ear/control/gateway.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <optional>
#include <utility>
#include <vector>

namespace ear::control {
namespace {

/// A node's health as last sent and when its phase began (main loop only). `told`: a node.state was sent for it; false
/// after node.added of a node added disabled, so NodeHost's first report is sent even though node.added said it (a
/// protocol 0 client reads a state only from node.state).
struct Sent {
	NodeHealth health;
	std::chrono::system_clock::time_point since;
	bool told = true;
};
using SentMap = std::map<NodeName, Sent, std::less<>>;
/// The companion list last sent for each node a client holds (main loop only): [] from its node.added, gone at its
/// node.removed. A name absent is not held.
using CompanionsMap = std::map<NodeName, std::vector<CompanionView>, std::less<>>;

/// The health as the wire compares it: `superseded` is never on the wire.
NodeHealth onWire(NodeHealth health) {
	health.superseded = false;
	return health;
}

} // namespace

struct MainLoopGateway::Impl {
	Executor& mainLoop;
	Executor& control;
	GraphRequests& requests;
	WallClock clock;
	SteadyClock steady;
	EventSink* sink = nullptr; // main loop only
	/// Each node's health as last sent (main loop only), forgotten when the node leaves the graph.
	std::unique_ptr<SentMap> sent = std::make_unique<SentMap>();
	/// The companion list last sent for each node a client holds (main loop only): [] at its node.added, erased at its
	/// node.removed (a replace's too).
	std::unique_ptr<CompanionsMap> companions = std::make_unique<CompanionsMap>();

	/// Main loop: `call(*sink)` on the control executor; dropped before watch().
	template <class Call> void toSink(Call call) {
		if (sink == nullptr) return;
		control.post([sink = sink, call = std::move(call)]() mutable { call(*sink); });
	}
	void post(GraphEvent event) {
		toSink([event = std::move(event)](EventSink& to) mutable { to.graphChanged(std::move(event)); });
	}
	/// A steady stamp moved onto the wall clock (docs/architecture.md#status-and-events: core reads no wall clock).
	std::chrono::system_clock::time_point onWall(std::chrono::steady_clock::time_point at) const {
		const auto ago = steady() - at;
		return clock() - std::chrono::duration_cast<std::chrono::system_clock::duration>(
							 ago > std::chrono::steady_clock::duration::zero()
								 ? ago
								 : std::chrono::steady_clock::duration::zero());
	}
	GestureRequests* gestures = nullptr;
	/// Control thread: the gestures whose end or cancel was read and not yet answered. Once one is answered its gesture
	/// is closed, aborted or was never open, and the main loop drops any later tick of it, so its id goes: the set holds
	/// only the ends and cancels in flight, whatever ids a client makes up.
	std::set<std::string, std::less<>> ending;
	/// The ticks not yet taken by the main loop, the newest per gesture (any thread, under its mutex).
	struct Mailbox {
		std::mutex mutex;
		std::map<std::string, std::pair<SessionToken, MoveGesture>> ticks;
		bool posted = false;
	};
	std::unique_ptr<Mailbox> mail = std::make_unique<Mailbox>();
	/// Main loop: each node's open gestures as their notes said them.
	std::unique_ptr<std::map<NodeName, std::vector<NodeGesture>, std::less<>>> onNodes =
		std::make_unique<std::map<NodeName, std::vector<NodeGesture>, std::less<>>>();
	/// Whether a client holds the node: added, and not removed since.
	bool held(const NodeName& node) const { return companions->contains(node); }
};

MainLoopGateway::MainLoopGateway(Executor& mainLoop, Executor& control, GraphRequests& requests, WallClock clock,
                                 SteadyClock steady)
	: impl_(std::make_unique<Impl>(Impl{mainLoop, control, requests, std::move(clock), std::move(steady)})) {}

MainLoopGateway::~MainLoopGateway() = default;

void MainLoopGateway::run(std::vector<Command> commands,
                          std::move_only_function<void(std::vector<CommandResult>)> done) {
	// The keeper calls the reply on the main loop after every event the batch caused was told, so
	// posting `done` from there puts it behind them in the control executor's queue.
	impl_->requests.submit(std::move(commands), [&control = impl_->control,
	                                             done = std::move(done)](std::vector<CommandResult> results) mutable {
		control.post([done = std::move(done), results = std::move(results)]() mutable { done(std::move(results)); });
	});
}

void MainLoopGateway::watch(EventSink& sink) {
	impl_->mainLoop.post([impl = impl_.get(), &sink] { impl->sink = &sink; });
}

void MainLoopGateway::graphFileChanged(const GraphFileChanged& change) {
	impl_->toSink([change](EventSink& to) mutable { to.graphFileChanged(std::move(change)); });
}

void MainLoopGateway::graphFileWritten(const std::string&) {}

void MainLoopGateway::nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) {
	const NodeHealth health = healthFromSpec(node);
	const auto since = impl_->clock();
	impl_->sent->insert_or_assign(node.name, Sent{health, since, !node.disabled});
	impl_->companions->insert_or_assign(node.name, std::vector<CompanionView>{}); // what node.added says
	impl_->post(NodeAdded{node, config->ports().publicOnly(), health, since, initialStatus(config->statusDeclared())});
}

void MainLoopGateway::nodeReplaced(const NodeSpec& node, std::shared_ptr<const NodeConfig> config,
                                   const NodeChange& change) {
	static_cast<void>(change); // a replace is of another type, a new node that counts from zero
	const NodeHealth health = healthFromSpec(node);
	const auto since = impl_->clock();
	impl_->sent->insert_or_assign(node.name, Sent{health, since, !node.disabled});
	impl_->companions->insert_or_assign(node.name, std::vector<CompanionView>{}); // what node.added says
	impl_->post(NodeAdded{node, config->ports().publicOnly(), health, since});
}

void MainLoopGateway::nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig>) {
	impl_->post(NodeUpdated{node.name, node.settings, node.disabled, NodeOutcome::restart});
}

void MainLoopGateway::nodeChanged(const NodeSpec& node, std::shared_ptr<const NodeConfig>, const NodeChange& change) {
	NodeUpdated updated{node.name, node.settings, node.disabled, change.outcome};
	for (const Retuned& retune : change.retuned) updated.retuned.push_back(retune.key);
	// A change of ports keeps the node, its block and the links of the ports it keeps (docs/architecture.md#reshapes):
	// the client redraws its handles in place (docs/control-port.md#notifications).
	if (change.portsChanged())
		updated.reshaped =
			NodeReshaped{change.inPlace, portsMoved(change.oldPorts, change.ports), change.ports.publicOnly()};
	impl_->post(std::move(updated));
}

void MainLoopGateway::nodeRemoved(const NodeName& node) {
	impl_->sent->erase(node);
	impl_->companions->erase(node); // a client drops the node here: nothing about its companions is sent after
	impl_->post(NodeRemoved{node});
}

void MainLoopGateway::routeAdded(const Route& route) { impl_->post(RouteAdded{route}); }

void MainLoopGateway::routeRemoved(const Route& route) { impl_->post(RouteRemoved{route}); }

void MainLoopGateway::nodeChanged(const NodeHealth& told) {
	NodeHealth health = onWire(told);
	auto known = impl_->sent->find(health.name);
	if (known != impl_->sent->end() && known->second.told && known->second.health == health) return;
	if (known == impl_->sent->end()) {
		known = impl_->sent->emplace(health.name, Sent{health, impl_->clock()}).first;
	} else {
		if (known->second.health.phase != health.phase) known->second.since = impl_->clock();
		known->second.health = health;
		known->second.told = true;
	}
	impl_->post(NodeChanged{std::move(health), known->second.since});
}

void MainLoopGateway::nodeGone(const NodeName& node, std::uint64_t droppedFrames) {
	impl_->sent->erase(node);
	impl_->post(NodeStopped{node, droppedFrames});
	impl_->post(NodeGone{node, droppedFrames});
}

void MainLoopGateway::routeChanged(const RouteStatus& status) { impl_->post(RouteChanged{status}); }

void MainLoopGateway::countersChanged(const GraphCounts& graph, std::uint64_t lostMs,
                                      std::span<const NamedCounts> nodes) {
	impl_->post(CountersChanged{graph, lostMs, {nodes.begin(), nodes.end()}});
}

void MainLoopGateway::companionsChanged(const NodeName& node, std::span<const CompanionView> companions) {
	// NodeHost compares lists without their counters and CounterWatch only with the lists it told itself, so either may
	// tell one the other already did (a structural change carrying a counter that moved, then CounterWatch's beat).
	// A name no client holds (removed, and not added again since) is dropped whatever its list says.
	const auto known = impl_->companions->find(node);
	if (known == impl_->companions->end()) return;
	if (std::ranges::equal(known->second, companions)) return;
	known->second.assign(companions.begin(), companions.end());
	impl_->post(NodeCompanionsChanged{node, {companions.begin(), companions.end()}});
}

void MainLoopGateway::statusChanged(const NodeName& node, std::span<const FieldValue> fields) {
	if (impl_->held(node)) impl_->post(NodeStatusChanged{node, {fields.begin(), fields.end()}});
}

void MainLoopGateway::statusReset(const NodeName& node, std::span<const FieldValue> fields) {
	if (impl_->held(node)) impl_->post(NodeStatusChanged{node, {fields.begin(), fields.end()}, true});
}

void MainLoopGateway::eventRaised(const NodeName& node, const RaisedEvent& event) {
	if (impl_->held(node)) impl_->post(NodeEventRaised{node, event, impl_->onWall(event.at)});
}

void MainLoopGateway::nodeDiagnostic(const NodeName& node, const NodeDiagnostic& diagnostic) {
	if (impl_->held(node)) impl_->post(NodeDiagnosed{node, diagnostic, impl_->onWall(diagnostic.at)});
}

void MainLoopGateway::answerGestures(GestureRequests& gestures) { impl_->gestures = &gestures; }

void MainLoopGateway::gesture(GestureCommand command, SessionToken session,
                              std::move_only_function<void(GestureAnswer)> done) {
	Impl& impl = *impl_;
	std::optional<std::string> ended;
	if (const auto* end = std::get_if<EndGesture>(&command)) ended = end->gesture;
	if (const auto* cancel = std::get_if<CancelGesture>(&command)) ended = cancel->gesture;
	// The answer crosses back to the control executor (a notification's is dropped), and an end's or a cancel's id
	// leaves `ending` there, behind it.
	auto replyOnControl = [&impl, ended, done = std::move(done)](GestureAnswer answer) mutable {
		if (!done && !ended) return;
		impl.control.post(
			[&impl, ended = std::move(ended), done = std::move(done), answer = std::move(answer)]() mutable {
				if (ended) impl.ending.erase(*ended);
				if (done) done(std::move(answer));
			});
	};
	if (auto* move = std::get_if<MoveGesture>(&command)) {
		if (impl.ending.contains(move->gesture)) return; // its end or cancel was read: no tick after it
		// The mailbox: the newest tick per gesture, one main-loop task for however many arrive before it runs
		// (docs/control-port.md#gestures).
		bool post = false;
		{
			const std::lock_guard lock(impl.mail->mutex);
			impl.mail->ticks.insert_or_assign(move->gesture, std::pair{session, std::move(*move)});
			post = !std::exchange(impl.mail->posted, true);
		}
		if (post)
			impl.mainLoop.post([&impl] {
				std::map<std::string, std::pair<SessionToken, MoveGesture>> ticks;
				{
					const std::lock_guard lock(impl.mail->mutex);
					ticks.swap(impl.mail->ticks);
					impl.mail->posted = false;
				}
				if (impl.gestures == nullptr) return;
				for (auto& [id, tick] : ticks) impl.gestures->move(tick.first, tick.second);
			});
		return;
	}
	if (ended) impl.ending.insert(*ended);
	impl.mainLoop.post([&impl, command = std::move(command), session, reply = std::move(replyOnControl)]() mutable {
		if (impl.gestures == nullptr) {
			reply(GestureRefused{std::string(unknownGestureCode), "gestures are not served", nlohmann::json::object()});
			return;
		}
		if (const auto* begin = std::get_if<BeginGesture>(&command)) reply(impl.gestures->begin(session, *begin));
		else if (const auto* cancel = std::get_if<CancelGesture>(&command))
			reply(impl.gestures->cancel(session, *cancel));
		else if (const auto* end = std::get_if<EndGesture>(&command))
			impl.gestures->end(session, *end, std::move(reply));
	});
}

void MainLoopGateway::ending(const std::string& gesture) { impl_->ending.insert(gesture); }

void MainLoopGateway::sessionClosed(SessionToken session) {
	impl_->mainLoop.post([&impl = *impl_, session] {
		if (impl.gestures != nullptr) impl.gestures->sessionClosed(session);
	});
}

void MainLoopGateway::gestureChanged(const GestureNote& note) {
	// The graph's per-node gestures (docs/control-port.md#gesturechanged), kept for each snapshot's stamp.
	auto& on = *impl_->onNodes;
	switch (note.phase) {
	case GesturePhase::begin:
		for (const GestureKey& at : note.reach) on[at.node].push_back({note.gesture, at.key, note.value.value_or(0.0)});
		break;
	case GesturePhase::change:
		for (auto& [node, gestures] : on)
			for (NodeGesture& held : gestures)
				if (held.gesture == note.gesture) held.value = note.value.value_or(held.value);
		break;
	case GesturePhase::end:
	case GesturePhase::aborted:
		for (auto it = on.begin(); it != on.end();) {
			std::erase_if(it->second, [&](const NodeGesture& held) { return held.gesture == note.gesture; });
			it = it->second.empty() ? on.erase(it) : std::next(it);
		}
		break;
	case GesturePhase::tickRefused:
		break;
	}
	impl_->toSink([note](EventSink& to) mutable { to.gestureChanged(std::move(note)); });
}

void MainLoopGateway::stamp(GraphView& view) const {
	for (NodeView& node : view.nodes) {
		if (const auto known = impl_->sent->find(node.spec.name); known != impl_->sent->end())
			node.since = known->second.since;
		if (const auto held = impl_->onNodes->find(node.spec.name); held != impl_->onNodes->end())
			node.gestures = held->second;
		node.diagnosticsAt.clear();
		for (const NodeDiagnostic& diagnostic : node.diagnostics)
			node.diagnosticsAt.push_back(impl_->onWall(diagnostic.at));
	}
}

} // namespace ear::control
