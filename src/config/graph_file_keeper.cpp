// The graph file keeper as ear wires it (ear/config/graph_file_keeper.hpp): the sequencer (Keeper)
// runs on its own thread (KeeperThread), judged on the main loop by its main-loop side, over ear.conf
// in the graph file folder. The keeper's own calls reach it only as work posted to that thread, so
// the knobly working copy lives there alone; join() touches it from the main thread only once the
// thread has ended.
#include "ear/config/graph_file_keeper.hpp"

#include <stdexcept>
#include <utility>

#include "graph_file_disk.hpp"
#include "graph_file_model.hpp"
#include "keeper.hpp"
#include "keeper_thread.hpp"
#include "main_loop_side.hpp"

namespace ear::config {

struct FirstLoad::Impl {
	DiskText base;
	GraphFileModel model;
};

FirstLoad::FirstLoad(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
FirstLoad::FirstLoad(FirstLoad&& other) noexcept = default;
FirstLoad& FirstLoad::operator=(FirstLoad&& other) noexcept = default;
FirstLoad::~FirstLoad() = default;

std::optional<AudioFormat> FirstLoad::format() const noexcept { return impl_->model.read().format; }

namespace {

/// The keeper's own executor: posts to its thread once it runs (nothing is posted before start()).
class ThreadExecutor final : public Executor {
public:
	void post(std::move_only_function<void()> work) override {
		if (thread != nullptr) thread->post(std::move(work));
	}
	KeeperThread* thread = nullptr;
};

} // namespace

struct GraphFileKeeper::Impl {
	Impl(FirstLoad::Impl first, const folder::ConfinedFolder& folder, AudioFormat engine, DesiredGraph& graph,
	     const GraphReader& reader, NodeOperations& operations, Executor& mainLoop)
		: folder(folder), disk(folder), side(graph, reader, operations),
		  keeper(std::move(first.base), std::move(first.model), disk, engine, side, mainLoop, self) {}

	const folder::ConfinedFolder& folder;
	FolderGraphFileDisk disk;
	MainLoopSide side;
	ThreadExecutor self;
	Keeper keeper;
	std::unique_ptr<KeeperThread> thread;
	bool joined = false;
	Gestures* gestures = nullptr;
	bool stopping = false; // main loop
};

FirstLoad GraphFileKeeper::load(const folder::ConfinedFolder& folder, const NodeCatalogue& catalogue) {
	FolderGraphFileDisk disk(folder);
	DiskText base = disk.read();
	GraphFileModel model = GraphFileModel::load(base, catalogue);
	return FirstLoad(std::make_unique<FirstLoad::Impl>(FirstLoad::Impl{std::move(base), std::move(model)}));
}

GraphFileKeeper::GraphFileKeeper(FirstLoad first, const folder::ConfinedFolder& folder, AudioFormat engine,
                                 DesiredGraph& graph, const GraphReader& reader, NodeOperations& operations,
                                 Executor& mainLoop)
	: impl_(std::make_unique<Impl>(std::move(*first.impl_), folder, engine, graph, reader, operations, mainLoop)) {}

GraphFileKeeper::~GraphFileKeeper() {
	if (!impl_->joined) join();
}

void GraphFileKeeper::subscribe(control::GraphFileListener& listener) { impl_->side.subscribe(listener); }

void GraphFileKeeper::subscribeActions(control::ActionListener& listener) { impl_->side.subscribeActions(listener); }

void GraphFileKeeper::answerActions(NodeActions& actions) { impl_->side.answerActions(actions); }

void GraphFileKeeper::stampSnapshots(const control::SnapshotStamp& stamp) { impl_->side.stampSnapshots(stamp); }

std::expected<void, std::string> GraphFileKeeper::start() {
	if (impl_->thread) throw std::logic_error("GraphFileKeeper::start called twice");
	Keeper& keeper = impl_->keeper;
	auto thread =
		KeeperThread::start(impl_->folder.heldPath(), std::string(rootDocument),
	                        KeeperThread::Handlers{[&keeper] { keeper.diskEvent(); }, [&keeper] { keeper.free(); }});
	if (!thread) return std::unexpected(std::move(thread.error()));
	impl_->thread = std::move(*thread);
	impl_->self.thread = impl_->thread.get();
	impl_->thread->post([&keeper] { keeper.start(); });
	return {};
}

void GraphFileKeeper::submit(std::vector<control::Command> commands, control::BatchReply reply) {
	if (!impl_->thread) throw std::logic_error("GraphFileKeeper::submit before start");
	impl_->thread->post([&keeper = impl_->keeper, commands = std::move(commands), reply = std::move(reply)]() mutable {
		keeper.request(std::move(commands), std::move(reply));
	});
}

void GraphFileKeeper::holdGestures(Gestures& gestures) {
	impl_->gestures = &gestures;
	impl_->side.holdGestures(gestures);
	gestures.releaseThrough([this](GestureEnding ending) { commit(std::move(ending), nullptr); });
}

void GraphFileKeeper::commit(GestureEnding ending, std::move_only_function<void(control::GestureAnswer)> reply) {
	std::vector<control::Command> commands;
	const std::string id = ending.gesture;
	commands.emplace_back(control::CommitGesture{std::move(ending)});
	submit(std::move(commands),
	       [this, id, reply = std::move(reply)](std::vector<control::CommandResult> results) mutable {
			   control::CommandResult result = std::move(results.at(0));
			   // Any refusal of an end aborts its gesture, so a release is never left open (docs/architecture.md#gestures).
			   if (!std::holds_alternative<control::GestureEnded>(result) && impl_->gestures != nullptr)
				   impl_->gestures->abort(id, "refused");
			   if (reply) reply(control::GestureAnswer{std::move(result)});
		   });
}

control::GestureAnswer GraphFileKeeper::begin(SessionToken session, const control::BeginGesture& begin) {
	return impl_->side.beginGesture(session, begin, impl_->stopping);
}

void GraphFileKeeper::move(SessionToken session, const control::MoveGesture& move) {
	if (impl_->gestures != nullptr && !impl_->stopping)
		impl_->gestures->move(move.gesture, session, move.seq, move.value);
}

void GraphFileKeeper::end(SessionToken session, const control::EndGesture& end,
                          std::move_only_function<void(control::GestureAnswer)> reply) {
	if (impl_->stopping || impl_->gestures == nullptr)
		return reply(control::CommandResult{std::expected<void, CommandError>{std::unexpected(Stopping{})}});
	auto ending = impl_->gestures->end(end.gesture, end.value, session);
	if (!ending)
		return reply(control::GestureRefused{std::string(control::unknownGestureCode), "unknown-gesture " + end.gesture,
		                                     nlohmann::json::object()});
	commit(std::move(*ending), std::move(reply));
}

control::GestureAnswer GraphFileKeeper::cancel(SessionToken session, const control::CancelGesture& cancel) {
	if (impl_->gestures == nullptr || !impl_->gestures->cancel(cancel.gesture, session))
		return control::GestureRefused{std::string(control::unknownGestureCode), "unknown-gesture " + cancel.gesture,
		                               nlohmann::json::object()};
	return control::GestureCancelled{};
}

void GraphFileKeeper::sessionClosed(SessionToken session) {
	if (impl_->gestures != nullptr && !impl_->stopping) impl_->gestures->sessionClosed(session);
}

void GraphFileKeeper::stop() {
	// A stop is nobody's release: open gestures are dropped, not committed (docs/architecture.md#gestures).
	impl_->stopping = true;
	if (impl_->gestures != nullptr) impl_->gestures->drop();
	if (!impl_->thread) return;
	impl_->thread->post([&keeper = impl_->keeper] { keeper.stop(); });
	impl_->thread->unwatch(); // a stopped keeper takes no disk event; its descriptor's close overlaps the nodes' stop
}

std::optional<std::string> GraphFileKeeper::join() {
	if (!impl_->joined) {
		impl_->joined = true;
		if (impl_->thread) impl_->thread->stop(); // runs Keeper::stop and every completion already posted
		impl_->keeper.finish();                   // the thread has ended: the last write, if it has not run
	}
	return impl_->keeper.finalWriteFailure();
}

} // namespace ear::config
