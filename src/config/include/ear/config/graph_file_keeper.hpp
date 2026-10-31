// Interface page: docs/design/knobly-config.md (slice S4). The graph file keeper as ear wires it:
// ear.conf read once at boot on the main thread, then kept on one background thread that takes every
// request batch, judges it through the desired graph on the main loop, watches the graph file folder
// and writes ear.conf when it is free. No knobly type is in this header.
#pragma once
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "ear/control/requests.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/executor.hpp"
#include "ear/core/graph_view.hpp"
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
  std::optional<AudioFormat> format() const noexcept;   // when both lines read (GraphFileRead::format)
private:
  friend class GraphFileKeeper;
  struct Impl; std::unique_ptr<Impl> impl_;
  explicit FirstLoad(std::unique_ptr<Impl> impl) noexcept;
};
class GraphFileKeeper final : public control::GraphRequests {
public:
  /// Main thread, at boot. Never fails for the file's content. folder and catalogue outlive the result.
  static FirstLoad load(const folder::ConfinedFolder& folder, const NodeCatalogue& catalogue);
  /// Main thread. Nothing runs until start(). All references outlive the keeper.
  GraphFileKeeper(FirstLoad first, const folder::ConfinedFolder& folder, AudioFormat engine,
                  DesiredGraph& graph, const GraphReader& reader, Executor& mainLoop);
  GraphFileKeeper(const GraphFileKeeper&) = delete;
  GraphFileKeeper& operator=(const GraphFileKeeper&) = delete;
  ~GraphFileKeeper();   // join() if not joined
  void subscribe(control::GraphFileListener& listener);   // main thread, before start(); told in subscribe order
  /// Main thread, once, after Engine::create and before ControlPort::start (so submit always finds the
  /// thread). Starts the thread (watch first), and its first work is Keeper::start(). Error: the watch.
  std::expected<void, std::string> start();
  void submit(std::vector<control::Command> commands, control::BatchReply reply) override;   // posts Keeper::request
  void stop();   // main loop, after DesiredGraph::close(): posts Keeper::stop
  /// Main thread, after the main loop returned: stops and joins the thread (the last write runs
  /// first). Returns the last write's failure, for standard output.
  std::optional<std::string> join();
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
