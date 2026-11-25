// The graph file keeper's sequencer (keeper.hpp; docs/architecture.md#how-a-change-moves).
// One input at a time from one FIFO (batches, disk checks, the first offer); while a step awaits its
// completion nothing more is taken. A step is one main-loop post: the reads gathered so far, then at
// most one offer, then the reply if it is the batch's last step; its completion comes back to
// `self` with the verdict and the rest of the batch. Every post carries plain values only, so no
// knobly object leaves this thread.
#include "keeper.hpp"

#include <algorithm>
#include <format>
#include <random>
#include <stdexcept>
#include <utility>

namespace ear::config {
namespace {

template <class... Fs> struct Overloaded : Fs... {
	using Fs::operator()...;
};

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

/// A lifecycle verb that edits nothing (docs/control-port.md#methods): its verb and node.
std::optional<std::pair<control::LifecycleVerb, NodeName>> verbOf(const control::Command& command) {
	if (const auto* c = std::get_if<control::StartNode>(&command))
		return std::pair{control::LifecycleVerb::start, c->node};
	if (const auto* c = std::get_if<control::StopNode>(&command))
		return std::pair{control::LifecycleVerb::stop, c->node};
	if (const auto* c = std::get_if<control::RestartNode>(&command))
		return std::pair{control::LifecycleVerb::restart, c->node};
	return std::nullopt;
}

control::AppliedKind kindOf(NodeOutcome outcome) {
	switch (outcome) {
	case NodeOutcome::disable:
		return control::AppliedKind::disable;
	case NodeOutcome::enable:
		return control::AppliedKind::enable;
	case NodeOutcome::stored:
		return control::AppliedKind::stored;
	case NodeOutcome::restart:
		return control::AppliedKind::restart;
	case NodeOutcome::reconfigure:
		return control::AppliedKind::reconfigure;
	case NodeOutcome::reshape:
		return control::AppliedKind::reshape;
	case NodeOutcome::unchanged:
		break;
	}
	return control::AppliedKind::unchanged;
}

/// An accepted disable's, enable's or set's answer from the plan's change of its node (docs/control-port.md#methods): no
/// change is unchanged; node.set's carries, when the ports change, whether a reshape is in place, the ports added and
/// removed and the routes it removed (or, dry, would remove).
template <class Answer> CommandResult appliedOf(const Answer& answer, const std::optional<NodeChange>& change) {
	if (!change) return control::Applied{control::AppliedKind::unchanged};
	control::Applied applied{kindOf(change->outcome)};
	if (answer.detailed && change->portsChanged()) {
		if (change->outcome == NodeOutcome::reshape) applied.inPlace = change->inPlace;
		applied.ports = portsMoved(change->oldPorts, change->ports);
		if (answer.dryRun) applied.routes = answer.routes;
		else applied.routesDropped = answer.routes;
	}
	return applied;
}

/// The routes a candidate keeps that name ports its edit takes from `node`, and those ports, in the order the node
/// had them (docs/control-port.md#changes-of-ports). The keeper knows them on its own thread: a type's parse and ports
/// are pure.
struct Stranded {
	std::vector<PortName> ports;
	std::vector<Route> routes;
};
Stranded strandedBy(const GraphFileModel& before, const GraphFileModel& after, const NodeName& node) {
	Stranded out;
	const auto was = before.portsOf(node);
	const auto now = after.portsOf(node);
	if (!was || !now || !after.read().wanted) return out;
	const PortsMoved moved = portsMoved(*was, *now);
	if (moved.removed.empty()) return out;
	const auto gone = [&](const PortRef& end) {
		return end.node == node && std::ranges::find(moved.removed, end.port) != moved.removed.end();
	};
	for (const Route& route : after.read().wanted->routes)
		if ((gone(route.from) || gone(route.to)) && std::ranges::find(out.routes, route) == out.routes.end())
			out.routes.push_back(route);
	for (const PortName& port : moved.removed)
		if (std::ranges::any_of(out.routes, [&](const Route& r) {
				return (r.from.node == node && r.from.port == port) || (r.to.node == node && r.to.port == port);
			}))
			out.ports.push_back(port);
	return out;
}

/// A batch's whole reply, every result filled.
void replyWith(std::vector<std::optional<CommandResult>>& filled, control::BatchReply& reply) {
	std::vector<CommandResult> results;
	results.reserve(filled.size());
	for (auto& result : filled) results.push_back(std::move(*result));
	reply(std::move(results));
}

CommandResult refusalOf(const EditRefusal& refusal) {
	return std::visit(Overloaded{[](const CommandError& e) { return refused(e); },
	                             [](const control::FileRefusal& r) { return CommandResult{r}; }},
	                  refusal);
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

void Keeper::answered(Waiting& waiting, std::size_t index, CommandResult result) {
	--waiting.outstanding;
	if (!waiting.held) {
		waiting.early.emplace_back(index, std::move(result));
		return;
	}
	waiting.results[index] = std::move(result);
	if (waiting.outstanding == 0) replyWith(waiting.results, waiting.reply); // behind every event before it
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
			else reads.push_back({index, std::monostate{}});
			continue;
		}
		if (auto verb = verbOf(command)) {
			reads.push_back({index, std::move(*verb)});
			continue;
		}
		if (const auto* invoke = std::get_if<control::InvokeNode>(&command)) {
			reads.push_back({index, *invoke});
			continue;
		}
		if (auto answer = take(command)) {
			batch.results[index] = std::move(*answer);
			continue;
		}
		const GraphFileRead& read = candidate_->model.read();
		Offer offer{fileState(candidate_->model, candidate_->version),
		            *read.wanted,
		            read.placements,
		            control::FileChangeCause::edit,
		            0,
		            candidate_->answer.dryRun,
		            candidate_->answer.node};
		const bool last = batch.next == batch.commands.size();
		postStep(std::move(batch), std::move(reads), std::move(offer), index, candidate_->answer, last);
		return;
	}
	postStep(std::move(batch), std::move(reads), std::nullopt, 0, Answer{}, true);
}

std::optional<CommandResult> Keeper::take(const control::Command& command) {
	if (stopped_) return refused(Stopping{});
	std::optional<EditOutcome> edit;
	Answer answer;
	if (const auto* set = std::get_if<control::SetGraphFile>(&command)) {
		if (set->version != version()) return CommandResult{control::FileRefusal{control::FileChanged{}}};
		if (workingFromText_ && set->text == working_.text()) return done();
		edit = EditOutcome{std::optional<GraphFileModel>{working_.fresh(DiskText{set->text})}};
	} else if (const auto* replace = std::get_if<control::SetNodeText>(&command)) {
		// The box's text was cut from `version` (docs/control-port.md#a-nodes-own-text): stale is file-changed first.
		if (replace->version != version()) return CommandResult{control::FileRefusal{control::FileChanged{}}};
		if (working_.read().hasErrors()) return CommandResult{control::FileRefusal{control::FileInvalid{}}};
		edit = working_.setNodeText(replace->node, replace->text);
	} else if (const auto* add = std::get_if<control::AddNodeText>(&command)) {
		// Written against the file's @set and @snippet lines at `version` (docs/control-port.md#a-nodes-own-text).
		if (add->version != version()) return CommandResult{control::FileRefusal{control::FileChanged{}}};
		if (working_.read().hasErrors()) return CommandResult{control::FileRefusal{control::FileInvalid{}}};
		edit = working_.addNodeText(add->text);
	} else if (const auto* merge = std::get_if<control::SetNode>(&command)) {
		// node.set: node.update with the node's settings merged, made at the source like every edit
		// (docs/control-port.md#nodeset-and-dry-runs).
		if (working_.read().hasErrors()) return CommandResult{control::FileRefusal{control::FileInvalid{}}};
		const auto& nodes = working_.read().wanted->nodes;
		const auto held = std::ranges::find(nodes, merge->node, &NodeSpec::name);
		if (held == nodes.end()) return refused(UnknownNode{merge->node});
		if (!merge->settings.is_object()) return refused(InvalidSettings{merge->node, "settings: must be an object"});
		NodeSettings merged = held->settings;
		merged.merge_patch(merge->settings); // null removes a key
		answer = Answer{merge->node, true, merge->dryRun, {}};
		edit = working_.updateNode(merge->node, merged);
	} else {
		if (working_.read().hasErrors()) return CommandResult{control::FileRefusal{control::FileInvalid{}}};
		edit = std::visit(Overloaded{
							  [&](const control::AddNode& c) { return working_.addNode(c.node); },
							  [&](const control::UpdateNode& c) { return working_.updateNode(c.node, c.settings); },
							  [&](const control::RemoveNode& c) { return working_.removeNode(c.node); },
							  [&](const control::AddRoute& c) { return working_.addRoute(c.route); },
							  [&](const control::RemoveRoute& c) { return working_.removeRoute(c.route); },
							  [&](const control::DisableNode& c) {
								  answer.node = c.node;
								  return working_.disableNode(c.node);
							  },
							  [&](const control::EnableNode& c) {
								  answer.node = c.node;
								  return working_.enableNode(c.node);
							  },
							  [](const auto&) -> EditOutcome { throw std::logic_error("keeper: not an edit"); },
						  },
		                  command);
	}
	if (!*edit) return refusalOf(edit->error());
	if (!**edit) return answer.node ? CommandResult{control::Applied{control::AppliedKind::unchanged}} : done();
	GraphFileModel model = std::move(***edit);
	// An edit's or a set's candidate that does not read cannot be planned: the text's errors answer it.
	if (model.read().hasErrors())
		return CommandResult{control::FileRefusal{control::InvalidText{model.read().diagnostics}}};
	std::optional<std::pair<NodeName, bool>> settingsOf; // a node's settings edit, and whether it drops routes
	if (const auto* update = std::get_if<control::UpdateNode>(&command))
		settingsOf = {update->node, update->dropRoutes};
	if (const auto* merge = std::get_if<control::SetNode>(&command))
		settingsOf = {merge->node, merge->dropRoutes || merge->dryRun};
	if (settingsOf) {
		// A route to a port the edit takes away is refused ports-in-use, naming every such route, unless the edit
		// asks dropRoutes: then their lines go in the same candidate, one commit and one write. A dry run names them
		// as the routes it would remove (docs/control-port.md#changes-of-ports).
		Stranded stranded = strandedBy(working_, model, settingsOf->first);
		if (!stranded.routes.empty()) {
			if (!settingsOf->second)
				return CommandResult{control::ControlRefusal{
					control::PortsInUse{settingsOf->first, std::move(stranded.ports), std::move(stranded.routes)}}};
			auto without = model.removeRoutes(stranded.routes);
			if (!without) return refusalOf(without.error());
			if (*without) model = std::move(**without);
			answer.routes = std::move(stranded.routes);
		}
	}
	candidate_ = Candidate{std::move(model), nextVersion(), std::move(answer)};
	return std::nullopt;
}

void Keeper::postStep(Batch batch, std::vector<Read> reads, std::optional<Offer> offer, std::size_t offerIndex,
                      Answer answer, bool last) {
	awaiting_ = true;
	mainLoop_.post([&side = side_, &self = self_, keeper = this, batch = std::move(batch), reads = std::move(reads),
	                offer = std::move(offer), offerIndex, answer = std::move(answer), last,
	                stopped = stopped_]() mutable {
		for (Read& read : reads) {
			if (auto* invoke = std::get_if<control::InvokeNode>(&read.what)) {
				// Handed on, never waited for: its answer lands in the batch's waiting results, now or later
				// (docs/control-port.md#actions).
				++batch.waiting->outstanding;
				side.invoke(invoke->node, invoke->action, invoke->args, stopped,
				            [waiting = batch.waiting, index = read.index](CommandResult result) mutable {
								answered(*waiting, index, std::move(result));
							});
				continue;
			}
			batch.results[read.index] =
				std::visit(Overloaded{
							   [&](std::monostate) { return CommandResult{side.graph()}; },
							   [&](FileState& file) { return CommandResult{side.graphFile(std::move(file))}; },
							   [&](std::pair<control::LifecycleVerb, NodeName>& verb) {
								   return side.operate(verb.first, verb.second, stopped);
							   },
							   [](control::InvokeNode&) -> CommandResult { throw std::logic_error("keeper: invoke"); },
						   },
			               read.what);
		}
		std::optional<Verdict> verdict;
		if (offer) {
			Judged judged = side.offer(std::move(*offer));
			verdict = judged.verdict;
			if (answer.node && judged.verdict == Verdict::accepted)
				batch.results[offerIndex] = appliedOf(answer, judged.change);
			else batch.results[offerIndex] = CommandResult{std::move(judged.result)};
		}
		std::optional<Batch> rest;
		if (last) {
			Waiting& waiting = *batch.waiting;
			for (auto& [index, result] : waiting.early) batch.results[index] = std::move(result);
			waiting.early.clear();
			if (waiting.outstanding > 0) { // held on the main loop until its last action answered
				waiting.results = std::move(batch.results);
				waiting.reply = std::move(batch.reply);
				waiting.held = true;
			} else {
				replyWith(batch.results, batch.reply);
			}
		} else {
			rest = std::move(batch);
		}
		self.post([keeper, verdict, rest = std::move(rest)]() mutable { keeper->completed(verdict, std::move(rest)); });
	});
}

void Keeper::completed(std::optional<Verdict> verdict, std::optional<Batch> batch) {
	awaiting_ = false;
	if (candidate_) {
		if (verdict == Verdict::accepted && !candidate_->answer.dryRun) { // a dry run is never kept
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
	if (working_.read().hasErrors()) announce(control::FileChangeCause::disk, droppedEdits);
	else offerDisk(droppedEdits);
}

void Keeper::offerDisk(std::uint32_t droppedEdits) {
	const GraphFileRead& read = working_.read();
	Offer offer{fileState(working_, version()), *read.wanted, read.placements, control::FileChangeCause::disk,
	            droppedEdits};
	awaiting_ = true;
	mainLoop_.post([&side = side_, &self = self_, keeper = this, offer = std::move(offer)]() mutable {
		const Verdict verdict = side.offer(std::move(offer)).verdict;
		self.post([keeper, verdict] { keeper->completed(verdict, std::nullopt); });
	});
}

void Keeper::announce(control::FileChangeCause cause, std::uint32_t droppedEdits) {
	mainLoop_.post([&side = side_, file = fileState(working_, version()), cause, droppedEdits]() mutable {
		side.announce(std::move(file), cause, droppedEdits);
	});
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
	if (std::holds_alternative<folder::Changed>(*outcome)) return; // the hand edit wins
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
	return FileState{version, model.text(), read.diagnostics, std::move(pending), writeFailed_, read.nodeTexts};
}

} // namespace ear::config
