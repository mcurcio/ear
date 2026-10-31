// The graph file keeper's sequencer (docs/design/knobly-config.md, design rules 1-3; S4): it owns
// the working copy, the disk base and the write state, takes requests, verdicts and disk checks one
// at a time in arrival order, and writes ear.conf only when nothing is waiting. Keeper thread only
// (every call, including the completions it posts to `self`). Its tests drive it on one thread with
// two pumped executors.
#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "ear/control/requests.hpp"
#include "ear/core/executor.hpp"
#include "graph_file_disk.hpp"
#include "graph_file_model.hpp"
#include "main_loop_side.hpp"
namespace ear::config {
class Keeper {
public:
  /// `base`: what boot read; `first`: its load. All references outlive the keeper. `self` runs this keeper's calls.
  Keeper(DiskText base, GraphFileModel first, GraphFileDisk& disk, AudioFormat engine,
         MainLoopSide& side, Executor& mainLoop, Executor& self);
  void start();      // queues the first offer (cause disk, planned from empty), then a disk check
  void request(std::vector<control::Command> commands, control::BatchReply reply);
  void diskEvent();  // ear.conf may have changed: queues one disk check (coalesced with one already queued)
  void free();       // nothing posted is waiting: write if ahead (rules below)
  void stop();       // edits from now on answer Stopping; after the queue drains, one last write if ahead
  /// After the keeper's thread ended (no completion will arrive): a step still awaited and the queue
  /// are dropped, and the last write runs if it has not. For a main loop that quit with a step in it.
  void finish();
  std::optional<std::string> finalWriteFailure() const;   // the last write's detail, after stop

private:
  struct Batch {
    std::vector<control::Command> commands;
    std::size_t next = 0;                                   // the first command not yet taken
    std::vector<std::optional<control::CommandResult>> results;
    control::BatchReply reply;
  };
  struct FirstOffer {};
  struct DiskCheck {};
  using Input = std::variant<FirstOffer, DiskCheck, Batch>;
  struct Read { std::size_t index; std::optional<FileState> file; };   // file: GetGraphFile; none: GetGraph
  struct Candidate { GraphFileModel model; std::string version; };

  void pump();
  void advance(Batch batch);
  /// A keeper-made answer to command `index`, or nothing when it made a candidate (left in candidate_).
  std::optional<control::CommandResult> take(const control::Command& command);
  void postStep(Batch batch, std::vector<Read> reads, std::optional<Offer> offer, std::size_t offerIndex, bool last);
  void completed(std::optional<Verdict> verdict, std::optional<Batch> batch);
  void checkDisk(DiskText disk);
  void present(std::uint32_t droppedEdits);   // the working copy after a disk load: announced or offered
  void offerDisk(std::uint32_t droppedEdits);
  void announce(control::FileChangeCause cause, std::uint32_t droppedEdits);
  bool ahead() const;
  void write();
  void finishOnce();
  void setWorking(GraphFileModel model, bool fromText);
  void setBase(DiskText disk);
  std::string version() const;       // the working copy's
  std::string nextVersion() const;   // a candidate's (it always differs from the working copy's text)
  FileState fileState(const GraphFileModel& model, const std::string& version) const;

  GraphFileDisk& disk_;
  AudioFormat engine_;
  MainLoopSide& side_;
  Executor& mainLoop_;
  Executor& self_;
  std::string bootId_;
  std::uint64_t n_ = 1;
  GraphFileModel working_;
  bool workingFromText_;
  DiskText base_;
  std::uint32_t unwritten_ = 0;
  std::optional<std::string> writeFailed_;
  bool writeFailedIsBase_ = false;   // writeFailed_ holds the base's own error
  bool retryBlocked_ = false;        // a write failed; retried after the next accepted change or disk event
  std::deque<Input> fifo_;
  bool diskCheckQueued_ = false;
  bool started_ = false;            // nothing is taken before start()
  bool awaiting_ = false;
  std::optional<Candidate> candidate_;
  bool stopped_ = false;
  bool finished_ = false;
  std::optional<std::string> finalWriteFailure_;
};
}
