// The graph file keeper as ear wires it (ear/config/graph_file_keeper.hpp; S4 criterion 17): a real
// thread, a real folder, and a main loop that is a blocking queue the test runs. Every wait is for
// an event; the deadline only fails the test.
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>

#include "catch2/catch.hpp"
#include "ear/config/graph_file_keeper.hpp"
#include "keeper_support.hpp"
#include "test_types.hpp"

using namespace ear;
using namespace ear::config;
using namespace ear::config::test;
using namespace std::chrono_literals;

namespace {

/// The main loop: work posted from any thread, run by the test on its own thread.
class BlockingQueue final : public Executor {
public:
	void post(std::move_only_function<void()> work) override {
		{
			const std::scoped_lock lock(mutex_);
			queue_.push_back(std::move(work));
		}
		ready_.notify_one();
	}
	/// Runs posted work until `done` holds; false at the deadline.
	bool runUntil(const std::function<bool()>& done) {
		const auto deadline = std::chrono::steady_clock::now() + 10s;
		while (!done()) {
			std::unique_lock lock(mutex_);
			if (!ready_.wait_until(lock, deadline, [&] { return !queue_.empty(); })) return false;
			auto work = std::move(queue_.front());
			queue_.pop_front();
			lock.unlock();
			work();
		}
		return true;
	}

private:
	std::mutex mutex_;
	std::condition_variable ready_;
	std::deque<std::move_only_function<void()>> queue_;
};

class NoReader final : public GraphReader {
public:
	GraphView read() const override { return {}; }
};

} // namespace

TEST_CASE("The graph file keeper on its thread: start announces, an edit replies and is written, stop then join",
          "[graph_file_keeper]") {
	TempDir dir("facade");
	const auto file = dir.path() / "ear.conf";
	std::ofstream(file, std::ios::binary) << recordExample;
	auto folder = folder::ConfinedFolder::open(dir.path());
	REQUIRE(folder.has_value());

	NodeCatalogue catalogue{testTypes()};
	DesiredGraph graph{catalogue};
	NoReader reader;
	BlockingQueue mainLoop;
	Log log;
	FileLog fileLog{log};

	auto first = GraphFileKeeper::load(*folder, catalogue);
	REQUIRE(first.format().has_value());
	CHECK(first.format()->rate == 48000);
	GraphFileKeeper keeper(std::move(first), *folder, {48000, 256}, graph, reader, mainLoop);
	keeper.subscribe(fileLog);
	REQUIRE(keeper.start().has_value());

	REQUIRE(mainLoop.runUntil([&] { return !fileLog.changes.empty(); }));
	CHECK(fileLog.changes[0].cause == control::FileChangeCause::disk);
	CHECK(fileLog.changes[0].file.running == fileLog.changes[0].file.version);
	CHECK(graph.nodes().size() == 4);

	std::optional<std::vector<control::CommandResult>> reply;
	keeper.submit({control::UpdateNode{"rec", {{"path", "take2.wav"}}}},
	              [&](std::vector<control::CommandResult> results) { reply = std::move(results); });
	REQUIRE(mainLoop.runUntil([&] { return reply.has_value(); }));
	REQUIRE(reply->size() == 1);
	CHECK(std::get<std::expected<void, CommandError>>((*reply)[0]).has_value());
	const auto edited = fileLog.changes.back().file.version;

	REQUIRE(mainLoop.runUntil([&] { return !fileLog.written.empty(); }));
	CHECK(fileLog.written == std::vector<std::string>{edited});
	CHECK(contents(file) == replaced(recordExample, "take1.wav", "take2.wav"));

	graph.close();
	keeper.stop();
	CHECK_FALSE(keeper.join().has_value());
	CHECK(contents(file) == replaced(recordExample, "take1.wav", "take2.wav"));
}
