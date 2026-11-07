// Frozen interface (docs/design/batch2.md section 4; GraphCycle and InsidesEvents by docs/design/sendspin.md section 14,
// S1; delayFrames by its erratum 22; PortBank and Ports' banks by batch2.md section 11's erratum of 2026-11-06; NodeType
// answering from one table by its erratum "NodeType, 2026-11-06"; StartContext, the closed list of resource requests,
// named inside banks, the pass-through, the two-phase start and the run object by docs/design/node-framework.md
// sections 7.2 and 7.3, slice 1). Change it only through a design page and a re-cut, never a local edit.
#pragma once
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include "ear/core/command_error.hpp"
#include "ear/core/rtp_receiver.hpp"
#include "ear/core/sound_card.hpp"
namespace ear {
/// One named group of a node's ports, all one way. Each is "<name>.<member>" (portName). A routable bank's ports are
/// public: routes name them. An inside bank's ports belong to the port block only: nothing reaches them but the node's
/// own outside nodes (its sound cards and RTP receivers), which name the bank they join, and the node's code. Both are
/// named alike, so no type spells a port's name or indexes a list of ports by hand.
struct PortBank {
	enum class Reach { routable, inside };
	Direction direction;
	std::string_view name;
	std::vector<std::string> members;
	Reach reach = Reach::routable;
};
/// A port's whole name, the one place it is made: "<bank>.<member>", each part 1 or more of [A-Za-z0-9_-]. Throws
/// std::logic_error otherwise: a type bug, not a user error.
PortName portName(std::string_view bank, std::string_view member);
/// "1".."<count>": a numbered bank's members.
std::vector<std::string> numbered(std::uint32_t count);
/// A node's ports, declared only by banks. Invariant: bank names are unique (std::logic_error otherwise: a type bug, not
/// a user error), so the four lists' whole names are unique together.
class Ports {
public:
	/// One declared bank: its ports' whole names, and where its first port sits in the spans AudioCode::process gets
	/// (inputs then insideInputs, outputs then insideOutputs).
	struct Bank {
		std::string name;
		Direction direction;
		PortBank::Reach reach;
		std::vector<PortName> ports;
		std::size_t first;
		friend bool operator==(const Bank&, const Bank&) = default;
	};
	Ports() = default;
	explicit Ports(const std::vector<PortBank>& banks);
	/// Whole names, the banks' in declaration order, each bank's members in order.
	const std::vector<PortName>& inputs() const noexcept { return inputs_; }
	const std::vector<PortName>& outputs() const noexcept { return outputs_; }
	const std::vector<PortName>& insideInputs() const noexcept { return insideInputs_; }
	const std::vector<PortName>& insideOutputs() const noexcept { return insideOutputs_; }
	/// Every bank, in declaration order.
	const std::vector<Bank>& banks() const noexcept { return banks_; }
	/// The bank of this name. Throws std::logic_error when there is none: a type bug.
	const Bank& bank(std::string_view name) const;
	/// These ports without the inside ones: what the graph read and the wire show.
	Ports publicOnly() const;
	friend bool operator==(const Ports&, const Ports&) = default;

private:
	std::vector<PortName> inputs_;
	std::vector<PortName> outputs_;
	std::vector<PortName> insideInputs_;
	std::vector<PortName> insideOutputs_;
	std::vector<Bank> banks_;
};
/// Why a node is failed. Part of health; never returned by a control call.
struct FailureReason {
	enum class Code { engineRefused, startFailed };
	Code code;
	std::string detail;
};
std::string_view failureCode(FailureReason::Code code) noexcept; // "engine-refused" | "start-failed"
struct AudioFormat {
	std::uint32_t rate;
	std::uint32_t maxBlock;
}; // fixed at boot in M1

/// This cycle of the graph, on the driver's clock.
struct GraphCycle {
	std::int64_t startNs;   // its start, CLOCK_MONOTONIC
	std::uint64_t position; // the driver's frames at its start
	std::uint32_t delayFrames =
		0; // how long what this cycle writes waits in the driver before it is heard (0: no device)
};

class AudioCode {
public:
	virtual ~AudioCode() = default;
	/// Main thread, before the first process(), never concurrently with it. May allocate.
	virtual void prepare(const AudioFormat& format) = 0;
	/// Realtime: no locks, allocation, file/network access or waiting. `inputs` = inputs then
	/// insideInputs, `outputs` = outputs then insideOutputs, each in declaration order, each span
	/// exactly `frames` long, frames <= maxBlock. An unlinked input reads zeros. Every output
	/// sample must be written.
	virtual void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	                     std::uint32_t frames, const GraphCycle& cycle) noexcept = 0;
	/// Main thread, after the last process() returned.
	virtual void release() noexcept = 0;
};
/// Realtime end of a handoff. Valid until the run's teardown closed the handoff.
class RtWriter {
public:
	/// Realtime thread only. Interleaves `frames` frames of planar `channels` (size == the
	/// handoff's channel count, each span >= frames) into the ring. Never blocks, locks or
	/// allocates. Returns frames accepted; the rest are dropped and counted by the framework.
	virtual std::uint32_t write(std::span<const std::span<const float>> channels, std::uint32_t frames) noexcept = 0;

protected:
	~RtWriter() = default;
};
/// Ordinary-thread end of a handoff, implemented by the node type.
class HandoffConsumer {
public:
	virtual ~HandoffConsumer() = default;
	/// The handoff's own thread (never realtime or main). May block on I/O. In write order;
	/// interleaved.size() == frames * channels.
	virtual void consume(std::span<const float> interleaved, std::uint32_t frames) = 0;
	/// Same thread, exactly once, after the last consume().
	virtual void finish() = 0;
};
/// A handoff from one input bank (routable or inside) of the node: one channel per port of the bank, `capacityFrames`
/// of ring. `consumer` must live until the run is destroyed (in the run object, or in what its recipe holds).
struct HandoffRequest {
	std::string bank;
	std::uint32_t capacityFrames;
	HandoffConsumer* consumer;
};
/// Every kind of resource a run can acquire: a closed list, on purpose, since a kind changes what the framework
/// promises (docs/design/node-framework.md section 7.3). Each kind has one adapter (node_host.hpp).
using ResourceRequest = std::variant<SoundCardRequest, RtpReceiverRequest, HandoffRequest>;
/// What a run tells, from any of its threads; never blocks. Told on the main loop in order, and only to the run that
/// wrote it: nothing after its close's `closed`, and nothing of an older run lands on a newer one
/// (docs/design/node-framework.md section 6, I6).
class InsidesEvents {
public:
	virtual void say(std::string text) = 0;    // a note: `node <n> says <text>`; never a state
	virtual void lost(std::string detail) = 0; // `failed start-failed: <detail>`; ignored once the run's close began
protected:
	~InsidesEvents() = default;
};
/// What `start` sees (docs/design/node-framework.md section 7.2): the format, the node's name, and the acquire calls.
/// Main thread, inside start() only: any call outside it throws std::logic_error. Everything acquired belongs to the run
/// and is torn down by the framework by dependency (section 6, I5): outside nodes, then handoffs, then the run object.
class StartContext {
public:
	virtual AudioFormat format() const = 0; // the value prepare() will get
	virtual const NodeName& node() const = 0;
	/// A sound card joined to `request.bank`, an inside output bank (playback) or inside input bank (capture): opened at
	/// format().rate with one channel per port of the bank, ready once its PCM runs and every channel is linked, and
	/// lost (the node fails engine-refused) when it fails, before or after it was ready.
	void acquire(const SoundCardRequest& request) { acquireResource(request); }
	/// An RTP receiver joined to `request.bank`, an inside input bank, with one channel per port of it; readiness, loss
	/// and teardown as a sound card's.
	void acquire(const RtpReceiverRequest& request) { acquireResource(request); }
	/// A handoff (HandoffRequest); its writer for the run's code. Ready at once.
	RtWriter& acquire(const HandoffRequest& request) { return *std::get<RtWriter*>(acquireResource(request)); }
	/// The run's events, until its close's `closed`; may be kept by the run's threads. (Kept on the start context
	/// until sendspin builds its run in the recipe, slice 3.)
	virtual InsidesEvents& events() = 0;

protected:
	~StartContext() = default;
	/// What an acquire hands back: nothing, or a handoff's writer.
	using Acquired = std::variant<std::monostate, RtWriter*>;
	/// Every engine object one run makes is named "<node>.<suffix>"; a second request with a suffix already taken in
	/// this run, a bank the node does not have or one of the wrong reach or direction throws std::logic_error (a type
	/// bug): docs/design/node-framework.md section 6, I9.
	virtual Acquired acquireResource(const ResourceRequest& request) = 0;
};
/// What the run object's recipe is handed once every resource of the run is ready (docs/design/node-framework.md
/// section 7.2). Main thread, inside the recipe only.
class Ready {
public:
	virtual AudioFormat format() const = 0;
	virtual InsidesEvents& events() = 0; // as StartContext::events
protected:
	~Ready() = default;
};
/// The type's own object for one run, with the threads it owns. Built by the recipe on the main loop, closed by the
/// framework at teardown (section 6, I5 step 4) and destroyed on the main loop after `closed`.
class RunObject {
public:
	virtual ~RunObject() = default;
	/// Main thread, once, after every outside node of the run is gone and every handoff closed. A run object with
	/// threads stops them (off the main loop: the main loop never joins a thread) and calls `closed`, last, from any
	/// thread; one without calls it at once. A run that has not called it within the close deadline is leaked, never
	/// destroyed (I5a).
	virtual void close(std::move_only_function<void()> closed) { closed(); }
};
/// One pair of a pass-through: each port of `from` (an input bank, routable or inside) copied to the port of `to` (an
/// output bank) at the same member index. The banks have as many ports as each other.
struct BankPair {
	std::string from;
	std::string to;
};
/// What runs on the realtime thread for one run: a type's own AudioCode, or the framework's pass-through between
/// banks (outputs no pair names are silent). The pass-through is resolved against the node's ports when the run starts;
/// a pair naming a bank the node lacks, of the wrong direction or of another size throws std::logic_error there.
class RunCode {
public:
	template <std::derived_from<AudioCode> Code> RunCode(std::unique_ptr<Code> code) noexcept
		: code_(std::move(code)) {} // NOLINT: implicit on purpose
	static RunCode passThrough(std::vector<BankPair> pairs);
	/// The type's own code; null for a pass-through.
	std::unique_ptr<AudioCode>& own() noexcept { return code_; }
	const std::vector<BankPair>& pairs() const noexcept { return pairs_; }

private:
	RunCode() = default;
	std::unique_ptr<AudioCode> code_;
	std::vector<BankPair> pairs_;
};
/// The framework's pass-through between banks, one pair or more: `card` and `rtp` write no audio code.
inline RunCode passThrough(std::vector<BankPair> pairs) { return RunCode::passThrough(std::move(pairs)); }
/// Builds the run object once every resource is ready, or fails the node (startFailed).
using RunRecipe = std::move_only_function<std::expected<std::unique_ptr<RunObject>, FailureReason>(Ready&)>;
/// What a start that acquired everything returns: the run's code, which drives the block at once, and optionally the
/// recipe of its run object, called once every resource is ready; the node is running once it returned.
struct Started {
	RunCode code;
	RunRecipe recipe = nullptr;
};
using StartResult = std::expected<Started, FailureReason>;
/// One node's checked settings; made only by its type's parse().
class NodeConfig {
public:
	virtual ~NodeConfig() = default;
	virtual const Ports& ports() const = 0; // pure; the only place this node's ports are declared
	/// Main thread; acquires only (docs/design/node-framework.md section 7.2). Error: the node becomes failed with this
	/// reason (startFailed).
	virtual StartResult start(StartContext& context) const = 0;
};
/// How one setting is written in the graph file: the keeper reads it with knobly's reader of this
/// kind and writes a JSON value of this kind back. The one home of each key's kind.
enum class SettingKind {
	text,        // JSON string; written quoted (knobly's quote), read with knobly's string reader
	wholeNumber, // JSON integer; written as a bare word, read with knobly's integer reader
};
struct SettingDeclaration {
	std::string_view key;
	SettingKind kind;
};
class AnySettingsTable; // settings_table.hpp
class CheckedSettings;  // settings_table.hpp
/// What a type makes of one node's checked settings: its config, or the whole text of a refusal. parse names the node.
using MadeConfig = std::expected<std::unique_ptr<const NodeConfig>, std::string>;
/// One node type. Its name, its settings' kinds and their checking all answer from the one table NodeTypeOf
/// (settings_table.hpp) bound it to; none can be overridden, so they cannot disagree.
class NodeType {
public:
	virtual ~NodeType() = default;
	NodeType(const NodeType&) = delete; // or a class could derive by copying a real type
	NodeType& operator=(const NodeType&) = delete;
	std::string_view name() const noexcept; // the NodeSpec `type`: the table's name
	/// Pure; the only place settings are checked; unknown keys are invalid. The table's walk, then the type's make; a
	/// refusal from either becomes InvalidSettings{node, text}.
	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings> parse(const NodeName& node,
	                                                                        const NodeSettings& settings) const;
	/// Every key parse() accepts, each once, with its kind, in row order; the same span every call; pure.
	std::span<const SettingDeclaration> settingKinds() const noexcept;

private:
	template <const auto& Table> friend class NodeTypeOf; // nothing else can derive
	explicit NodeType(const AnySettingsTable& table) noexcept;
	/// The one customisation point. Called only with what the table's walk made.
	virtual MadeConfig makeConfig(const NodeName& node, CheckedSettings settings) const = 0;
	const AnySettingsTable* table_; // static storage, because NodeTypeOf takes it as a template argument
};
} // namespace ear
