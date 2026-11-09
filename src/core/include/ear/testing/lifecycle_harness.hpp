// Step L-4a's harness (docs/design/node-framework.md 7.6, the class's home; docs/design/node-lifecycle-modules.md 3.8,
// the home of these declarations): one real NodeHost over in-memory fakes (D-L11), so a type's tests and NodeHost
// cannot disagree on any rule. The test's thread stands in for the main loop and the realtime thread both: every call
// pumps the harness's manual executor until nothing is queued, so each returns with the transition it asked for
// applied as far as the fakes allow.
//
// INTERIM LOCATION. type_harness.hpp and node_harness.hpp still declare slice 2's harnesses, which today's tests use, so
// these live in ear::testing::next until step L-4a: L-4a replaces node_harness.hpp's NodeHarness with this NodeHarness
// and type_harness.hpp's TypeHarness with this TypeHarness (dropping `next`), deletes this file, and rewrites
// testing/node_harness.cpp over NodeHost. L-3d renames ear::lifecycle::LifecycleReply to ear::LifecycleReply; the
// spelling here follows whichever is merged.
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include "ear/core/desired_graph.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/node_machines.hpp"
#include "ear/core/sink.hpp"
namespace ear::testing::next {

/// One chunk a sink's fake handed its consumer (on the test's thread, at once), copied.
struct SeenChunk {
	std::vector<float> interleaved;
	std::uint32_t frames;
	ChunkStamp stamp;
};

/// What the fakes made for one engine object or sink the configuration asked for, by bank, in request order.
struct CompanionFake {
	enum class Kind { soundCard, rtpReceiver, sink };
	Kind kind;
	std::string bank;
	NodeName engineName; // "<node>.playback", "<node>.capture", "<node>.rtp"; a sink's "<node>.<bank>"
	std::variant<SoundCardRequest, RtpReceiverRequest, SinkShape> request;
	bool made = false;             // an outside node: the flush made it (the block was ready); a sink: at once
	bool ready = false;            // an outside node: ready(bank) told it; a sink: its consumer began
	bool destroyed = false;        // an outside node destroyed, or a sink closed (its consumer finished)
	std::vector<SeenChunk> chunks; // a sink's, in order
};

/// One framework diagnostic the node got (NodeDiagnosticListener), in order.
struct SeenDiagnostic {
	std::string code;
	std::string detail;
	std::vector<std::pair<std::string, std::int64_t>> fields;
};

/// How a harness runs (outside the class, so a default argument can use its member initializers).
struct HarnessOptions {
	AudioFormat format = {.rate = 48000, .maxBlock = 256};
	NodeName node = "node";
	RecordingAdapter* recordings = nullptr; // null: every recording is refused ("no recordings folder"); until 3c
	bool autoComplete = true;               // fades acknowledged and barriers completed as soon as posted
};
/// How add() adds the node.
struct AddOptions {
	bool disabled = false;
};

/// Any NodeType, by names: what the all-type tests use (they take types from allNodeTypes, typeless), and the legacy
/// types' tests (sendspin's NodeTypeOf, hand-made test types) through the legacy adapter.
class NodeHarness {
public:
	using Options = HarnessOptions;
	/// `type` and `options.recordings` outlive the harness.
	explicit NodeHarness(const NodeType& type, Options options = {});
	/// Removes a node left in the graph and runs its teardown to the end (completing what autoComplete left), so no
	/// thread of a sink or a legacy run outlives the harness.
	~NodeHarness();
	NodeHarness(const NodeHarness&) = delete;
	NodeHarness& operator=(const NodeHarness&) = delete;

	// Settings.
	/// The type's parse: nothing, or the refusal's whole text. The settings add() and change() use.
	std::optional<std::string> parse(const NodeSettings& settings);
	/// The parsed node's ports, inside banks included; std::logic_error before a parse that passed.
	const Ports& ports() const;

	// Operations and the lifecycle's steps (node-framework.md 6.3), one call per transition.
	using AddOptions = ear::testing::next::AddOptions;
	/// T1 and, once the fake block is made, T2: NodeHost::nodeAdded; the instance made, configure called, readiness as
	/// far as the fakes allow. {.disabled = true}: T19, nothing configured. std::logic_error after no or a refused parse.
	void add(AddOptions options = {});
	/// The fake block's PortBlockEvents::ready: T5 follows when wanted running (the drive, then start()).
	void blockReady();
	/// The fake block's PortBlockEvents::lost: T13.
	void blockLost(std::string detail);
	/// The outside node on `bank` becomes ready; std::logic_error when none was made for it.
	void ready(std::string_view bank);
	/// The outside node on `bank` is lost: the node fails engine-refused with `detail`.
	void lose(std::string_view bank, std::string detail);
	/// With autoComplete off: the fade-out posted is acknowledged; the request posted (a drive or a silence) completes.
	/// std::logic_error when none is pending.
	void acknowledgeFade();
	void completeBarrier();
	/// The fake clock moves on; timers due fire (the 100 ms, 5 s and 10 s bounds, waitingFor's 2 s).
	void advance(std::chrono::milliseconds by);
	/// One cycle of `frames` (0: maxBlock) through the code the fake block was driven with, outputs NaN-filled first,
	/// with a GraphCycle the harness advances; then each bound sink's tap, whose chunk the sink fake hands its consumer.
	void process(std::uint32_t frames = 0);
	/// A committed change of settings (parse, then NodeHost::nodeChanged with outcome restart and the changed keys).
	NodeOutcome change(const NodeSettings& settings);
	lifecycle::LifecycleReply stop();    // NodeOperations::stop
	lifecycle::LifecycleReply start();   // NodeOperations::start
	lifecycle::LifecycleReply restart(); // NodeOperations::restart
	void disable();                      // nodeChanged, outcome disable (spec.disabled true)
	void enable();                       // nodeChanged, outcome enable
	void remove();                       // nodeRemoved

	// What the node shows.
	NodeHealth health() const;           // as last told
	std::string_view phase() const;      // phaseName of the last told health
	bool gone() const noexcept;          // nodeGone was told
	std::uint64_t droppedFrames() const; // nodeGone's count
	const std::vector<std::string>& notes() const noexcept;
	const std::vector<SeenDiagnostic>& diagnostics() const noexcept;

	// Buffers and fakes.
	/// A port's buffer (public or inside), maxBlock frames; std::logic_error for a port the node lacks.
	std::span<float> input(std::string_view port);
	std::span<const float> output(std::string_view port) const;
	/// Output samples of the last process() left unwritten (NaN), over every output port.
	std::size_t unwritten() const;
	/// The fade gain the fake block applied to `port` at the last sample of the last process() (1 when none).
	float lastFadeGain(std::string_view port) const;
	const std::vector<CompanionFake>& companions() const noexcept;
	/// The newest companion on `bank`; std::logic_error when none.
	CompanionFake& companion(std::string_view bank);
	/// Legacy only: frames handed through handoff `index` to its consumer, and the run object its recipe built.
	std::uint64_t handoffFrames(std::size_t index) const;
	RunObject* runObject() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

/// A type's own tests: NodeHarness over makeNodeType<Spec, Type>(), with banks named by the spec's handles, so a bank the
/// spec lacks does not compile.
template <const auto& Spec, class Type> class TypeHarness {
	template <class Bank> static constexpr bool ownBank = isBankHandle<Bank> && BanksOf<Spec>::template has<Bank>;

public:
	using Options = HarnessOptions;
	explicit TypeHarness(Options options = {})
		: type_(makeNodeType<Spec, Type>()), node_(std::make_unique<NodeHarness>(*type_, std::move(options))) {}
	const NodeType& type() const noexcept { return *type_; }
	NodeHarness& node() noexcept { return *node_; }
	NodeHarness* operator->() noexcept { return node_.get(); }

	std::optional<std::string> parse(const NodeSettings& settings) { return node_->parse(settings); }
	void add(AddOptions options = {}) { node_->add(options); }
	void blockReady() { node_->blockReady(); }
	void process(std::uint32_t frames = 0) { node_->process(frames); }
	NodeOutcome change(const NodeSettings& settings) { return node_->change(settings); }
	lifecycle::LifecycleReply stop() { return node_->stop(); }
	lifecycle::LifecycleReply start() { return node_->start(); }
	lifecycle::LifecycleReply restart() { return node_->restart(); }
	void disable() { node_->disable(); }
	void enable() { node_->enable(); }
	void remove() { node_->remove(); }
	std::string_view phase() const { return node_->phase(); }

	template <class Bank> void ready(Bank) {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		node_->ready(Bank::name);
	}
	template <class Bank> void lose(Bank, std::string detail) {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		node_->lose(Bank::name, std::move(detail));
	}
	template <class Bank> std::span<float> input(Bank, std::size_t member) {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		static_assert(Bank::direction == Direction::input, "input() takes an input bank");
		return node_->input(node_->ports().bank(Bank::name).ports.at(member));
	}
	template <class Bank> std::span<const float> output(Bank, std::size_t member) const {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		static_assert(Bank::direction == Direction::output, "output() takes an output bank");
		return node_->output(node_->ports().bank(Bank::name).ports.at(member));
	}
	template <class Bank> CompanionFake& companion(Bank) {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		return node_->companion(Bank::name);
	}
	template <class Bank> float lastFadeGain(Bank, std::size_t member = 0) const {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		static_assert(Bank::direction == Direction::output, "a fade is on an output bank");
		return node_->lastFadeGain(node_->ports().bank(Bank::name).ports.at(member));
	}

private:
	std::unique_ptr<const NodeType> type_;
	std::unique_ptr<NodeHarness> node_; // after the type it runs
};

} // namespace ear::testing::next
