// Running a node type without PipeWire (docs/design/node-framework.md section 7.6, slice 2): the one way a type's own
// tests and the all-type tests run it. NodeHarness takes any NodeType (the all-type tests get theirs from the generated
// list, typeless); TypeHarness (type_harness.hpp) adds the spec's bank handles. On one thread, the test's, standing in
// for the main loop and the realtime thread both: it parses settings and shows each refusal's text, calls start with
// fake resources (outside nodes that become ready or are lost when the test says, handoffs that hand each write
// straight to their consumer, recordings from a RecordingAdapter the test gives), builds the run object once every
// resource is ready, steps the run's code over buffers it owns, captures the run's notes and facts, and closes the run
// as the framework does (I5: release, outside nodes, handoffs, the run object).
#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "ear/core/node_host.hpp"
#include "ear/core/node_type.hpp"
namespace ear::testing {
class NodeHarness final : private StartContext, private Ready {
public:
	enum class State { unparsed, parsed, starting, running, failed, closed };
	/// `type` and `recordings` (null: every recording is refused, "no recordings folder") outlive the harness.
	explicit NodeHarness(const NodeType& type, AudioFormat format = {.rate = 48000, .maxBlock = 256},
	                     RecordingAdapter* recordings = nullptr, NodeName node = "node");
	/// Closes a run left open.
	~NodeHarness();
	NodeHarness(const NodeHarness&) = delete;
	NodeHarness& operator=(const NodeHarness&) = delete;

	// Settings.
	/// The type's parse: nothing, or the refusal's whole text (InvalidSettings' detail, the node named).
	std::expected<void, std::string> parse(const NodeSettings& settings);
	/// The parsed node's ports, inside banks included. Only after a parse that passed (else std::logic_error).
	const Ports& ports() const;

	// The run.
	/// start() with this harness as its StartContext. A failure is returned (and is failure()); else the code is
	/// prepared and, with no outside resource asked for, the recipe runs at once. Once per parse.
	std::expected<void, FailureReason> start();
	/// Every request start made, in order.
	const std::vector<ResourceRequest>& requests() const noexcept { return requests_; }
	/// Every latency start declared (StartContext::declareLatency), by bank, in order; refused as NodeHost refuses.
	const std::vector<std::pair<std::string, Latency>>& declaredLatency() const noexcept { return latency_; }
	/// The outside resources (sound cards, RTP receivers) among them, in order.
	std::size_t outsideCount() const noexcept { return outside_.size(); }
	/// The engine name the framework would give outside resource `index` ("<node>.<suffix>").
	const NodeName& outsideName(std::size_t index) const { return outside_.at(index).name; }
	/// Outside resource `index` runs: once every one does, the recipe builds the run object.
	void ready(std::size_t index);
	/// Outside resource `index` is lost: the node fails engine-refused with `detail`.
	void lose(std::size_t index, std::string detail);
	State state() const;
	/// Why the node failed, while state() is failed.
	std::optional<FailureReason> failure() const;
	/// The run object the recipe built; null with none.
	RunObject* runObject() const noexcept { return object_.get(); }

	// Audio.
	/// A port's buffer (public or inside), maxBlock frames: an input's to fill (zeros until written: an unlinked input),
	/// an output's to read after process(). Throws std::logic_error for a port the node lacks.
	std::span<float> input(std::string_view port);
	std::span<const float> output(std::string_view port) const;
	/// One cycle of `frames` (at most maxBlock) through the run's code. Every output is first filled with NaN, so
	/// unwritten() shows a sample the code did not write.
	void process(const GraphCycle& cycle, std::uint32_t frames);
	/// Output samples of the last process() left unwritten (NaN), over every output port.
	std::size_t unwritten() const;
	/// Frames handed through handoff `index` (in request order) to its consumer.
	std::uint64_t handoffFrames(std::size_t index) const;

	// What the run tells, from any thread.
	/// The run's notes (InsidesEvents::say), in order.
	std::vector<std::string> notes() const;

	/// The framework's teardown (I5): release(), the outside resources, each handoff's consumer finished, then the run
	/// object closed and waited for (std::runtime_error past 10 s), then the code, the run object and the recordings
	/// freed. Nothing of the run is heard after.
	void close();

private:
	struct Outside {
		NodeName name;
		bool ready = false;
	};
	struct Handoff;
	class Events;

	// StartContext and Ready.
	AudioFormat format() const override { return format_; }
	const NodeName& node() const override { return node_; }
	InsidesEvents& events() override;
	Acquired acquireResource(const ResourceRequest& request) override;
	void declareLatency(const std::string& bank, Latency internal) override;

	void buildIfAllReady();
	void fail(FailureReason reason);
	void applyFacts();

	const NodeType& type_;
	AudioFormat format_;
	RecordingAdapter* recordings_;
	NodeName node_;
	std::shared_ptr<const NodeConfig> config_;
	State state_ = State::unparsed;
	std::optional<FailureReason> failure_;
	bool inStart_ = false;
	bool inRecipe_ = false;
	std::vector<ResourceRequest> requests_;
	std::vector<std::pair<std::string, Latency>> latency_;
	std::vector<Outside> outside_;
	std::vector<std::unique_ptr<RecordingFile>> recordingFiles_;
	std::vector<std::unique_ptr<Handoff>> handoffs_;
	std::unique_ptr<AudioCode> code_;
	RunRecipe recipe_;
	std::unique_ptr<RunObject> object_;
	std::shared_ptr<Events> events_;
	std::vector<std::vector<float>> inputs_;  // inputs then inside inputs
	std::vector<std::vector<float>> outputs_; // outputs then inside outputs
	std::uint32_t lastFrames_ = 0;
};
} // namespace ear::testing
