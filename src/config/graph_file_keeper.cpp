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
	     const GraphReader& reader, Executor& mainLoop)
		: folder(folder), disk(folder), side(graph, reader),
		  keeper(std::move(first.base), std::move(first.model), disk, engine, side, mainLoop, self) {}

	const folder::ConfinedFolder& folder;
	FolderGraphFileDisk disk;
	MainLoopSide side;
	ThreadExecutor self;
	Keeper keeper;
	std::unique_ptr<KeeperThread> thread;
	bool joined = false;
};

FirstLoad GraphFileKeeper::load(const folder::ConfinedFolder& folder, const NodeCatalogue& catalogue) {
	FolderGraphFileDisk disk(folder);
	DiskText base = disk.read();
	GraphFileModel model = GraphFileModel::load(base, catalogue);
	return FirstLoad(std::make_unique<FirstLoad::Impl>(FirstLoad::Impl{std::move(base), std::move(model)}));
}

GraphFileKeeper::GraphFileKeeper(FirstLoad first, const folder::ConfinedFolder& folder, AudioFormat engine,
                                 DesiredGraph& graph, const GraphReader& reader, Executor& mainLoop)
	: impl_(std::make_unique<Impl>(std::move(*first.impl_), folder, engine, graph, reader, mainLoop)) {}

GraphFileKeeper::~GraphFileKeeper() {
	if (!impl_->joined) join();
}

void GraphFileKeeper::subscribe(control::GraphFileListener& listener) { impl_->side.subscribe(listener); }

std::expected<void, std::string> GraphFileKeeper::start() {
	if (impl_->thread) throw std::logic_error("GraphFileKeeper::start called twice");
	Keeper& keeper = impl_->keeper;
	auto thread = KeeperThread::start(impl_->folder.heldPath(), std::string(rootDocument),
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

void GraphFileKeeper::stop() {
	if (impl_->thread) impl_->thread->post([&keeper = impl_->keeper] { keeper.stop(); });
}

std::optional<std::string> GraphFileKeeper::join() {
	if (!impl_->joined) {
		impl_->joined = true;
		if (impl_->thread) impl_->thread->stop();   // runs Keeper::stop and every completion already posted
		impl_->keeper.finish();                     // the thread has ended: the last write, if it has not run
	}
	return impl_->keeper.finalWriteFailure();
}

} // namespace ear::config
