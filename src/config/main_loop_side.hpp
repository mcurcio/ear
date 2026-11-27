// The graph file keeper's main-loop side (docs/architecture.md#how-a-change-moves): the
// desired graph's only editor, and the one holder of the running version and the last refusal.
// Main loop only. It never parses text: what it judges arrives as plain values. operate, the lifecycle verbs' reply
// mapping and the action records are docs/control-port.md#methods.
#pragma once
#include <cstdint>
#include <memory>
#include <expected>
#include <optional>
#include <string>
#include <vector>
#include "ear/control/commands.hpp"
#include "ear/control/graph_file_state.hpp"
#include "ear/control/lifecycle_wire.hpp"
#include "ear/control/requests.hpp"
#include "ear/control/gestures_wire.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/gestures.hpp"
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
	/// Its reach index (docs/graph-file.md#reaches); none while the text has errors.
	std::shared_ptr<const ReachIndex> reach = nullptr;
};
/// What kind of change an offer is, as the related-change rule judges an open gesture by it
/// (docs/control-port.md#gestures-and-the-graph-file): a hand edit read from disk, a graphFile.set, any other edit, or a
/// gesture's end.
enum class OfferKind { disk, fileSet, edit, gestureEnd };
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
	OfferKind kind = OfferKind::disk;
	/// A gesture end's: the gesture, the value it commits, and a release's reason (docs/control-port.md#gestures).
	std::optional<std::string> gesture = std::nullopt;
	double gestureValue = 0.0;
	std::string gestureReason{};
};
enum class Verdict { accepted, refused, stopping };
struct Judged {
	Verdict verdict;
	std::expected<void, CommandError> result;
	std::optional<NodeChange> change{}; // an accepted offer's Plan::change(answerFor)
	/// Every other node's changed keys (its live ones among them) in that plan, in name order then key order:
	/// node.set's alsoChanged (docs/control-port.md#retunes).
	std::vector<control::NodeKey> alsoChanged{};
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
	/// Main thread, before the first offer: the gestures it judges each offer against and begins
	/// (docs/control-port.md#gestures).
	void holdGestures(Gestures& gestures);
	/// gesture.begin on the main loop (docs/control-port.md#gestures): stopping, file-invalid (the newest text it knows
	/// has errors), file-refused (the graph refused it), unknown-node, not-live, reach-mismatch and parameter-held, in
	/// that order, from the running version's reach index; else the gesture opened.
	control::GestureAnswer beginGesture(SessionToken session, const control::BeginGesture& begin, bool stopping);
	/// A gesture's end that changes nothing in the file: closed at `value`, answered with it.
	control::CommandResult closeGesture(const std::string& gesture, double value);
	/// Plan. Before the plan, each open gesture the offer relates to (docs/graph-file.md#reaches): an edit changing a
	/// held value is refused parameter-held; else, once accepted, each is aborted by its reason before the listeners
	/// are told, and its slots settle after the commit; a refused hand edit aborts every gesture. A gesture's end
	/// closes its gesture after the commit. Accepted: running = offer.file.version, listeners told graphFileChanged, commit. Refused
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
	void tell(FileState file, control::FileChangeCause cause, std::uint32_t droppedEdits,
	          std::optional<std::string> gesture = std::nullopt, std::string gestureReason = {});
	std::expected<std::vector<std::pair<std::string, std::string>>, CommandError> relatedTo(const Offer& offer) const;
	void abortAll(const std::string& reason);
	void newest(const FileState& file);
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
	Gestures* gestures_ = nullptr;
	std::shared_ptr<const ReachIndex> runningReach_; // the running version's
	bool newestHasErrors_ = false;                   // the newest text it was told of
	bool newestRefused_ = false;
};
} // namespace ear::config
