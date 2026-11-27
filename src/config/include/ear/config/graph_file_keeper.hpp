// Interface page: docs/graph-file.md. The graph file keeper as ear wires it:
// ear.conf read once at boot on the main thread, then kept on one background thread that takes every
// request batch, judges it through the desired graph on the main loop, watches the graph file folder
// and writes ear.conf when it is free. No knobly type is in this header. NodeOperations and subscribeActions are
// docs/control-port.md#methods.
#pragma once
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "ear/control/requests.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/gestures.hpp"
#include "ear/core/executor.hpp"
#include "ear/core/graph_view.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/node_catalogue.hpp"
#include "ear/core/node_type.hpp"
#include "ear/folder/confined_folder.hpp"
namespace ear::config {
/// ear.conf as read on the main thread at boot, before the keeper's thread starts.
class FirstLoad {
public:
	FirstLoad(FirstLoad&& other) noexcept;
	FirstLoad& operator=(FirstLoad&& other) noexcept;
	~FirstLoad();
	std::optional<AudioFormat> format() const noexcept; // when both lines read (GraphFileRead::format)
private:
	friend class GraphFileKeeper;
	struct Impl;
	std::unique_ptr<Impl> impl_;
	explicit FirstLoad(std::unique_ptr<Impl> impl) noexcept;
};
class GraphFileKeeper final : public control::GraphRequests, public control::GestureRequests {
public:
	/// Main thread, at boot. Never fails for the file's content. folder and catalogue outlive the result.
	static FirstLoad load(const folder::ConfinedFolder& folder, const NodeCatalogue& catalogue);
	/// Main thread. Nothing runs until start(). All references outlive the keeper.
	GraphFileKeeper(FirstLoad first, const folder::ConfinedFolder& folder, AudioFormat engine, DesiredGraph& graph,
	                const GraphReader& reader, NodeOperations& operations, Executor& mainLoop);
	GraphFileKeeper(const GraphFileKeeper&) = delete;
	GraphFileKeeper& operator=(const GraphFileKeeper&) = delete;
	~GraphFileKeeper();                                   // join() if not joined
	void subscribe(control::GraphFileListener& listener); // main thread, before start(); told in subscribe order
	/// Main thread, before start(): who writes each answered node.start, node.stop and node.restart.
	void subscribeActions(control::ActionListener& listener);
	/// Main thread, before start(): who runs each node.invoke (NodeHost: docs/control-port.md#actions).
	void answerActions(NodeActions& actions);
	/// Main thread, before start(): who fills each snapshot's `since` when the graph is read (the gateway).
	void stampSnapshots(const control::SnapshotStamp& stamp);
	/// Main thread, once, after Engine::create and before ControlPort::start (so submit always finds the
	/// thread). Starts the thread (watch first), and its first work is Keeper::start(). Error: the watch.
	std::expected<void, std::string> start();
	void submit(std::vector<control::Command> commands,
	            control::BatchReply reply) override; // posts Keeper::request
	/// Main thread, before start(): the gestures it begins, commits and judges every offer against, and commits the
	/// releases of (docs/control-port.md#gestures).
	void holdGestures(Gestures& gestures);
	/// GestureRequests, on the main loop (docs/control-port.md#gestures). An end, a gesture.end's or a release's, is a
	/// keeper step like an edit: its value at the definition, one commit; any refusal of it aborts the gesture
	/// (`refused`) before the reply.
	control::GestureAnswer begin(SessionToken session, const control::BeginGesture& begin) override;
	void move(SessionToken session, const control::MoveGesture& move) override;
	void end(SessionToken session, const control::EndGesture& end,
	         std::move_only_function<void(control::GestureAnswer)> reply) override;
	control::GestureAnswer cancel(SessionToken session, const control::CancelGesture& cancel) override;
	void sessionClosed(SessionToken session) override;
	/// Main loop, after DesiredGraph::close(): every gesture dropped, never committed, and later begins and ends refused
	/// stopping; posts Keeper::stop, then ends the folder watch and closes it.
	void stop();
	/// Main thread, after the main loop returned: stops and joins the thread (the last write runs
	/// first). Returns the last write's failure, for standard output.
	std::optional<std::string> join();

private:
	/// An end to commit through the keeper: its reply (none for a release), the gesture aborted on any refusal.
	void commit(GestureEnding ending, std::move_only_function<void(control::GestureAnswer)> reply);
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace ear::config
