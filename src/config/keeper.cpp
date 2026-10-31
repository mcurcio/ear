// The graph file keeper's sequencer (keeper.hpp; docs/design/knobly-config.md, design rules 1-3).
// One input at a time from one FIFO (batches, disk checks, the first offer); while a step awaits its
// completion nothing more is taken. A step is one main-loop post: the reads gathered so far, then at
// most one offer, then the reply if it is the batch's last step; its completion comes back to
// `self` with the verdict and the rest of the batch. Every post carries plain values only, so no
// knobly object leaves this thread.
#include "keeper.hpp"

#include <format>
#include <random>
#include <stdexcept>
#include <utility>

namespace ear::config {
namespace {

template <class... Fs> struct Overloaded : Fs... { using Fs::operator()...; };

using control::CommandResult;
using Outcome = std::expected<void, CommandError>;

CommandResult refused(CommandError error) { return CommandResult{Outcome{std::unexpected(std::move(error))}}; }
CommandResult done() { return CommandResult{Outcome{}}; }

/// 64 random bits in hex: versions from another run of ear never match this run's.
std::string makeBootId() {
	std::random_device random;
	const std::uint64_t bits = (std::uint64_t{random()} << 32) ^ std::uint64_t{random()};
	return std::format("{:016x}", bits);
}

bool isRead(const control::Command& command) {
	return std::holds_alternative<control::GetGraph>(command) || std::holds_alternative<control::GetGraphFile>(command);
}

} // namespace

Keeper::Keeper(DiskText base, GraphFileModel first, GraphFileDisk& disk, AudioFormat engine, MainLoopSide& side,
               Executor& mainLoop, Executor& self)
	: disk_(disk), engine_(engine), side_(side), mainLoop_(mainLoop), self_(self), bootId_(makeBootId()),
	  working_(std::move(first)), workingFromText_(base && base->has_value()), base_(std::move(base)) {
	if (!base_) {
		writeFailed_ = base_.error();
		writeFailedIsBase_ = true;
	}
}

void Keeper::start() {
	// First even when a folder event reached the thread before this call: its check waits behind it.
	started_ = true;
	fifo_.emplace_front(FirstOffer{});
	if (!diskCheckQueued_) {
		fifo_.emplace_back(DiskCheck{});
		diskCheckQueued_ = true;
	}
	pump();
}

void Keeper::request(std::vector<control::Command> commands, control::BatchReply reply) {
	Batch batch{std::move(commands), 0, {}, std::move(reply)};
	batch.results.resize(batch.commands.size());
	fifo_.emplace_back(std::move(batch));
	pump();
}

void Keeper::diskEvent() {
	if (stopped_) return;
	retryBlocked_ = false;
	if (!diskCheckQueued_) {
		fifo_.emplace_back(DiskCheck{});
		diskCheckQueued_ = true;
	}
	pump();
}

void Keeper::free() {
	if (stopped_ || awaiting_ || !fifo_.empty() || retryBlocked_ || !ahead()) return;
	write();
}

void Keeper::stop() {
	stopped_ = true;
	pump();
}

void Keeper::finish() {
	stopped_ = true;
	awaiting_ = false;
	candidate_.reset();
	fifo_.clear();
	diskCheckQueued_ = false;
	finishOnce();
}

std::optional<std::string> Keeper::finalWriteFailure() const { return finalWriteFailure_; }

void Keeper::pump() {
	if (!started_) return;
	while (!awaiting_ && !fifo_.empty()) {
		Input input = std::move(fifo_.front());
		fifo_.pop_front();
		std::visit(Overloaded{
		               [&](FirstOffer) { present(0); },
		               [&](DiskCheck) {
			               diskCheckQueued_ = false;
			               if (!stopped_) checkDisk(disk_.read());
		               },
		               [&](Batch& batch) { advance(std::move(batch)); },
		           },
		           input);
	}
	if (stopped_ && !awaiting_ && fifo_.empty()) finishOnce();
}

void Keeper::advance(Batch batch) {
	std::vector<Read> reads;
	while (batch.next < batch.commands.size()) {
		const std::size_t index = batch.next++;
		const control::Command& command = batch.commands[index];
		if (isRead(command)) {
			if (std::holds_alternative<control::GetGraphFile>(command))
				reads.push_back({index, fileState(working_, version())});
			else
				reads.push_back({index, std::nullopt});
			continue;
		}
		if (auto answer = take(command)) {
			batch.results[index] = std::move(*answer);
			continue;
		}
		const GraphFileRead& read = candidate_->model.read();
		Offer offer{fileState(candidate_->model, candidate_->version), *read.wanted, read.placements,
		            control::FileChangeCause::edit, 0};
		const bool last = batch.next == batch.commands.size();
		postStep(std::move(batch), std::move(reads), std::move(offer), index, last);
		return;
	}
	postStep(std::move(batch), std::move(reads), std::nullopt, 0, true);
}

std::optional<CommandResult> Keeper::take(const control::Command& command) {
	if (stopped_) return refused(Stopping{});
	std::optional<EditOutcome> edit;
	if (const auto* set = std::get_if<control::SetGraphFile>(&command)) {
		if (set->version != version()) return CommandResult{control::FileRefusal{control::FileChanged{}}};
		if (workingFromText_ && set->text == working_.text()) return done();
		edit = EditOutcome{std::optional<GraphFileModel>{working_.fresh(DiskText{set->text})}};
	} else {
		if (working_.read().hasErrors()) return CommandResult{control::FileRefusal{control::FileInvalid{}}};
		edit = std::visit(Overloaded{
		                      [&](const control::AddNode& c) { return working_.addNode(c.node); },
		                      [&](const control::UpdateNode& c) { return working_.updateNode(c.node, c.settings); },
		                      [&](const control::RemoveNode& c) { return working_.removeNode(c.node); },
		                      [&](const control::AddRoute& c) { return working_.addRoute(c.route); },
		                      [&](const control::RemoveRoute& c) { return working_.removeRoute(c.route); },
		                      [](const auto&) -> EditOutcome { throw std::logic_error("keeper: not an edit"); },
		                  },
		                  command);
	}
	if (!*edit)
		return std::visit(Overloaded{[](const CommandError& e) { return refused(e); },
		                             [](const control::FileRefusal& r) { return CommandResult{r}; }},
		                  edit->error());
	if (!**edit) return done();
	GraphFileModel model = std::move(***edit);
	// An edit's or a set's candidate that does not read cannot be planned: the text's errors answer it.
	if (model.read().hasErrors()) return CommandResult{control::FileRefusal{control::InvalidText{model.read().diagnostics}}};
	candidate_ = Candidate{std::move(model), nextVersion()};
	return std::nullopt;
}

void Keeper::postStep(Batch batch, std::vector<Read> reads, std::optional<Offer> offer, std::size_t offerIndex, bool last) {
	awaiting_ = true;
	mainLoop_.post([&side = side_, &self = self_, keeper = this, batch = std::move(batch), reads = std::move(reads),
	                offer = std::move(offer), offerIndex, last]() mutable {
		for (Read& read : reads)
			batch.results[read.index] = read.file ? CommandResult{side.graphFile(std::move(*read.file))} : CommandResult{side.graph()};
		std::optional<Verdict> verdict;
		if (offer) {
			Judged judged = side.offer(std::move(*offer));
			verdict = judged.verdict;
			batch.results[offerIndex] = CommandResult{std::move(judged.result)};
		}
		std::optional<Batch> rest;
		if (last) {
			std::vector<CommandResult> results;
			results.reserve(batch.results.size());
			for (auto& result : batch.results) results.push_back(std::move(*result));
			batch.reply(std::move(results));
		} else {
			rest = std::move(batch);
		}
		self.post([keeper, verdict, rest = std::move(rest)]() mutable { keeper->completed(verdict, std::move(rest)); });
	});
}

void Keeper::completed(std::optional<Verdict> verdict, std::optional<Batch> batch) {
	awaiting_ = false;
	if (candidate_) {
		if (verdict == Verdict::accepted) {
			setWorking(std::move(candidate_->model), true);
			++unwritten_;
			retryBlocked_ = false;
		}
		candidate_.reset();
	}
	if (batch) fifo_.push_front(std::move(*batch));
	pump();
}

void Keeper::checkDisk(DiskText disk) {
	if (disk == base_) return;
	const std::uint32_t dropped = unwritten_;
	unwritten_ = 0;
	setBase(std::move(disk));
	setWorking(working_.fresh(base_), base_ && base_->has_value());
	present(dropped);
}

void Keeper::present(std::uint32_t droppedEdits) {
	if (working_.read().hasErrors())
		announce(control::FileChangeCause::disk, droppedEdits);
	else
		offerDisk(droppedEdits);
}

void Keeper::offerDisk(std::uint32_t droppedEdits) {
	const GraphFileRead& read = working_.read();
	Offer offer{fileState(working_, version()), *read.wanted, read.placements,
	            control::FileChangeCause::disk, droppedEdits};
	awaiting_ = true;
	mainLoop_.post([&side = side_, &self = self_, keeper = this, offer = std::move(offer)]() mutable {
		const Verdict verdict = side.offer(std::move(offer)).verdict;
		self.post([keeper, verdict] { keeper->completed(verdict, std::nullopt); });
	});
}

void Keeper::announce(control::FileChangeCause cause, std::uint32_t droppedEdits) {
	mainLoop_.post([&side = side_, file = fileState(working_, version()), cause,
	                droppedEdits]() mutable { side.announce(std::move(file), cause, droppedEdits); });
}

bool Keeper::ahead() const {
	return workingFromText_ && base_.has_value() && *base_ != std::optional<std::string>(working_.text());
}

void Keeper::write() {
	auto outcome = disk_.replaceIf(*base_, working_.text());
	if (!outcome) {
		retryBlocked_ = true;
		if (writeFailed_ != outcome.error()) {
			writeFailed_ = std::move(outcome.error());
			writeFailedIsBase_ = false;
			announce(control::FileChangeCause::write, 0);
		}
		return;
	}
	if (auto* changed = std::get_if<folder::Changed>(&*outcome)) {
		checkDisk(DiskText{std::move(changed->current)});
		return;
	}
	base_ = std::optional<std::string>(working_.text());
	unwritten_ = 0;
	if (writeFailed_) {
		writeFailed_.reset();
		writeFailedIsBase_ = false;
		announce(control::FileChangeCause::write, 0);
	}
	mainLoop_.post([&side = side_, written = version()] { side.written(written); });
}

void Keeper::finishOnce() {
	if (finished_) return;
	finished_ = true;
	if (!ahead()) return;
	auto outcome = disk_.replaceIf(*base_, working_.text());
	if (!outcome) {
		finalWriteFailure_ = std::move(outcome.error());
		return;
	}
	if (std::holds_alternative<folder::Changed>(*outcome)) return;   // the hand edit wins
	base_ = std::optional<std::string>(working_.text());
	unwritten_ = 0;
	mainLoop_.post([&side = side_, written = version()] { side.written(written); });
}

void Keeper::setWorking(GraphFileModel model, bool fromText) {
	if (model.text() != working_.text()) ++n_;
	working_ = std::move(model);
	workingFromText_ = fromText;
}

void Keeper::setBase(DiskText disk) {
	base_ = std::move(disk);
	retryBlocked_ = false;
	if (!base_) {
		writeFailed_ = base_.error();
		writeFailedIsBase_ = true;
	} else if (writeFailedIsBase_) {
		writeFailed_.reset();
		writeFailedIsBase_ = false;
	}
}



std::string Keeper::version() const { return std::format("{}.{}", bootId_, n_); }

std::string Keeper::nextVersion() const { return std::format("{}.{}", bootId_, n_ + 1); }

FileState Keeper::fileState(const GraphFileModel& model, const std::string& version) const {
	const GraphFileRead& read = model.read();
	std::vector<control::FormatKey> pending;
	if (read.format) {
		if (read.format->rate != engine_.rate) pending.push_back(control::FormatKey::graphFrequency);
		if (read.format->maxBlock != engine_.maxBlock) pending.push_back(control::FormatKey::blockSize);
	}
	return FileState{version, model.text(), read.diagnostics, std::move(pending), writeFailed_};
}

} // namespace ear::config
