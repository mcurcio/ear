// The graph file keeper's main-loop side (docs/architecture.md#how-a-change-moves): the
// desired graph's only editor, and the one holder of the running version and the last refusal.
// Main loop only. It never parses text: what it judges arrives as plain values. operate, the lifecycle verbs' reply
// mapping and the action records are docs/control-port.md#methods.
#pragma once
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>
#include "ear/control/commands.hpp"
#include "ear/control/graph_file_state.hpp"
#include "ear/control/lifecycle_wire.hpp"
#include "ear/control/requests.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/graph_view.hpp"
#include "ear/core/node_host.hpp"
#include "graph_file_model.hpp"
namespace ear::config {
/// The working copy's file fields as they cross to the main loop (plain values).
struct FileState {
	std::string version;
	std::string text;
	std::vector<control::Diagnostic> diagnostics;
	std::vector<control::FormatKey> pendingRestart; // the file's format against the engine's, FormatKey order
	std::optional<std::string> writeFailed;
	std::optional<std::vector<control::NodeText>> nodeTexts = std::nullopt; // the read's (GraphFileRead::nodeTexts)
};
struct Offer {
	FileState file;
	GraphSpec wanted; // plan reads only nodes and routes
	Placements placements;
	control::FileChangeCause cause; // edit or disk
	std::uint32_t droppedEdits;     // disk only
	/// An edit's dry run (docs/control-port.md#nodeset-and-dry-runs): planned and answered, never committed, told,
	/// recorded or run.
	bool dryRun = false;
	/// The node whose change the reply answers with (node.disable, node.enable, node.set).
	std::optional<NodeName> answerFor = std::nullopt;
};
enum class Verdict { accepted, refused, stopping };
struct Judged {
	Verdict verdict;
	std::expected<void, CommandError> result;
	std::optional<NodeChange> change{}; // an accepted offer's Plan::change(answerFor)
};
/// Where a refusal points: the named node's line; the first route naming a refused node or port;
/// the route refused; the first node of an unknown type; the second of two nodes sharing a name. Pure.
std::optional<control::TextPlace> placeOf(const CommandError& error, const GraphSpec& wanted,
                                          const Placements& placements);
/// docs/control-port.md#methods table, its one home: core's reply as the wire's result, and as the action record.
control::CommandResult resultOf(const NodeName& node, const LifecycleReply& reply);
control::ActionRecord recordOf(control::LifecycleVerb verb, const NodeName& node, const LifecycleReply& reply);
/// docs/control-port.md#actions: core's answer to a type action as the wire's result, and as the action record.
control::CommandResult resultOf(const NodeName& node, std::string_view action, const ActionReply& reply);
control::ActionRecord recordOf(const NodeName& node, std::string_view action, const ActionReply& reply);
/// A type action's result, called once on the main loop: inside the call that asked, or later.
using ActionResult = std::move_only_function<void(control::CommandResult result)>;
class MainLoopSide {
public:
	MainLoopSide(DesiredGraph& graph, const GraphReader& reader, NodeOperations& operations); // all outlive it
	void subscribe(control::GraphFileListener& listener);     // main thread, before the first offer
	void subscribeActions(control::ActionListener& listener); // main thread, before the first offer
	/// Main thread, before the first offer: who fills each graph read's `since` (the gateway).
	void stampSnapshots(const control::SnapshotStamp& stamp);
	/// A lifecycle verb on the main loop: `stopping` (the keeper stopped) answers CommandError Stopping without asking;
	/// else NodeOperations' call with a judged callback that tells every ActionListener the record before any health
	/// change. Returns the reply mapped by docs/control-port.md#methods table (resultOf, recordOf).
	control::CommandResult operate(control::LifecycleVerb verb, const NodeName& node, bool stopping);
	/// Main thread, before the first offer: who runs node.invoke (NodeHost).
	void answerActions(NodeActions& actions);
	/// node.invoke on the main loop (docs/control-port.md#actions): `stopping` answers CommandError Stopping without
	/// asking; else NodeActions' invoke, whose answer (now, or once a call's handler answered) tells every
	/// ActionListener its record and then reaches `result`. No NodeActions given: a wiring bug, std::logic_error.
	void invoke(const NodeName& node, const std::string& action, const NodeSettings& args, bool stopping,
	            ActionResult result);
	/// Plan. Accepted: running = offer.file.version, listeners told graphFileChanged, commit. Refused
	/// with cause disk: the refusal recorded for that version (place from placeOf), listeners told.
	/// Refused edit: nothing told. Stopping: nothing told. A dry run: planned only, accepted or refused, nothing told,
	/// recorded or committed.
	Judged offer(Offer offer);
	/// A file change with no plan (errors; a write state): listeners told graphFileChanged.
	void announce(FileState file, control::FileChangeCause cause, std::uint32_t droppedEdits);
	/// `file` plus running and refusal. Refusal is present only when it was recorded for file.version.
	control::GraphFile graphFile(FileState file) const;
	GraphView graph() const;                  // reader.read(), stamped at once by every SnapshotStamp
	void written(const std::string& version); // listeners told graphFileWritten
private:
	struct Recorded {
		std::string version;
		control::Refusal refusal;
	};
	void tell(FileState file, control::FileChangeCause cause, std::uint32_t droppedEdits);
	void tellAction(const control::ActionRecord& record);
	DesiredGraph& graph_;
	const GraphReader& reader_;
	NodeOperations& operations_;
	NodeActions* actions_ = nullptr;
	std::vector<control::GraphFileListener*> listeners_;
	std::vector<control::ActionListener*> actionListeners_;
	std::vector<const control::SnapshotStamp*> stamps_;
	std::optional<std::string> running_;
	std::optional<Recorded> refusal_;
};
} // namespace ear::config
