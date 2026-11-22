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
#include "ear/core/cycle_facts.hpp"
#include "ear/core/latency.hpp" // Latency, BankPair
#include "ear/core/node_handle.hpp"
#include "ear/core/sample_rate.hpp"
#include "ear/core/store_rule.hpp"
namespace ear {
/// One named group of a node's ports, all one way. Each is "<name>.<member>" (portName). A routable bank's ports are
/// public: routes name them. An inside bank's ports belong to the port block only: nothing reaches them but the node's
/// own companions (its sound cards, RTP receivers, sinks and sources), each on the bank it joins, and the node's code.
/// Both are named alike, so no type spells a port's name or indexes a list of ports by hand.
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
/// Why a node is failed. Part of health; never returned by a control call. The codes are
/// docs/architecture.md#phases-states-and-what-is-wanted: `lost` is a node that had reached running or stopped and said
/// it cannot go on (its handle's lost(detail), or a hook that threw once it rested).
struct FailureReason {
	enum class Code { engineRefused, startFailed, lost };
	Code code;
	std::string detail;
	friend bool operator==(const FailureReason&, const FailureReason&) = default;
};
std::string_view failureCode(FailureReason::Code code) noexcept; // "engine-refused" | "start-failed" | "lost"
struct AudioFormat {
	SampleRate rate;
	std::uint32_t maxBlock;
}; // fixed at boot in M1

/// This cycle of the graph, on the driver's clock: ear's internal clock, the dummy driver, which always drives
/// (docs/engine.md#graphcycle). Made once a cycle by the framework, so no type reads a clock.
struct GraphCycle {
	std::int64_t startNs;        // its start, CLOCK_MONOTONIC
	std::uint64_t position;      // the driver's frames at its start
	std::int64_t startRawNs = 0; // the same start on CLOCK_MONOTONIC_RAW, the clock the driver tracks
	double rateDiff = 1.0;       // the driver's rate against the clock it tracks (its loop's correction)
	std::uint32_t driverId = 0;  // the driving node's engine id: another driver is another clock
	std::int64_t nextNs = 0;     // when the next cycle is due to start, CLOCK_MONOTONIC
	/// How long the next cycle had already been due when the block began this one (CLOCK_MONOTONIC, sampled by the
	/// framework with the clocks it reads for startRawNs); 0 when it was not yet due. Above 0 only while the driver
	/// catches up after a stall, in a burst of timeouts whose cycles start on its own time, behind raw time
	/// (docs/engine.md#the-internal-clock), whether the block runs each of them or misses some.
	std::int64_t overdueNs = 0;
	/// What this cycle states (docs/writing-a-node-type.md#lost-cycles): the cycle it ran in, what was missed and cut
	/// since the block's last, how far the position jumped, and why it is a discontinuity (facts.discontinuity()). The
	/// one home of every fact about lost cycles: a code reads them here, never works them out from `position`. Empty in
	/// a test's own cycle.
	CycleFacts facts{};
	/// One per output port (outputs then insideOutputs, as AudioCode::process's spans): the latency downstream of the
	/// port's bank, what the ports linked to the bank report (a following card: its buffer), combined over them. Empty
	/// when nothing tells it (a test's own cycle).
	std::span<const Latency> downstream{};
	/// When the block really began this cycle, on CLOCK_MONOTONIC_RAW (docs/engine.md#graphcycle): `startRawNs` when it
	/// began on time; when it began overdue, `startRawNs + (nextNs - startNs) + overdueNs`, the next cycle's due time
	/// plus how long that was already overdue, moved onto the raw clock. A `nextNs` of 0 (a test's own cycle) is on time.
	/// CLOCK_MONOTONIC and the raw clock differ by parts per million over the few milliseconds between: nanoseconds.
	constexpr std::int64_t ranRawNs() const noexcept {
		if (overdueNs <= 0 || nextNs == 0) return startRawNs;
		return startRawNs + (nextNs - startNs) + overdueNs;
	}
	/// The latency downstream of output `output`; none when it is not told.
	Latency downstreamOf(std::size_t output) const noexcept {
		return output < downstream.size() ? downstream[output] : Latency{};
	}
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
/// A type's code written against its spec (type_spec.hpp: `void process(Io<spec>, const GraphCycle&) noexcept`), not
/// an AudioCode: erased once, by eraseSpecCode, which type_spec.hpp defines (a type that names its code includes it).
template <class Code>
concept SpecCode = requires { &Code::process; } && !std::derived_from<Code, AudioCode> && std::move_constructible<Code>;
template <SpecCode Code> std::unique_ptr<AudioCode> eraseSpecCode(Code code);
/// What runs on the realtime thread for one configuration (docs/writing-a-node-type.md#the-code): a type's own code
/// (an AudioCode, or a code written against its spec), or the framework's pass-through between banks (outputs no pair
/// names are silent). The pass-through is resolved against the node's ports when configure returns; a pair naming a
/// bank the node lacks, of the wrong direction or of another size fails the node there.
class NodeCode {
public:
	template <std::derived_from<AudioCode> Code> NodeCode(std::unique_ptr<Code> code) noexcept
		: code_(std::move(code)) {} // NOLINT: implicit on purpose
	template <SpecCode Code> NodeCode(Code code)
		: code_(eraseSpecCode(std::move(code))) {} // NOLINT: implicit on purpose
	static NodeCode passThrough(std::vector<BankPair> pairs);
	/// The type's own code; null for a pass-through.
	std::unique_ptr<AudioCode>& own() noexcept { return code_; }
	const std::vector<BankPair>& pairs() const noexcept { return pairs_; }

private:
	NodeCode() = default;
	std::unique_ptr<AudioCode> code_;
	std::vector<BankPair> pairs_;
};
/// The framework's pass-through between banks, one pair or more: `card` and `rtp` write no audio code.
inline NodeCode passThrough(std::vector<BankPair> pairs) { return NodeCode::passThrough(std::move(pairs)); }
class ErasedInstance; // lifecycle.hpp
class NodeContext;    // lifecycle.hpp
/// One node's checked settings; made only by its type's parse().
class NodeConfig {
public:
	virtual ~NodeConfig() = default;
	virtual const Ports& ports() const = 0; // pure; the only place this node's ports are declared
	/// Main thread, at T2: one configuration's instance, which NodeHost drives (lifecycle.hpp); `node` outlives it.
	/// makeNodeType's config makes the type's class.
	virtual std::unique_ptr<ErasedInstance> make(NodeContext& node) const = 0;
};
/// How one setting is written in the graph file: the keeper reads it with knobly's reader of this
/// kind and writes a JSON value of this kind back. The one home of each key's kind.
enum class SettingKind {
	/// JSON string; written quoted (knobly's quote), read with knobly's string reader.
	text,
	/// JSON integer; written as a bare word, read with knobly's integer reader.
	wholeNumber,
	/// JSON integer of hertz; written with its unit (rateText: 44.1khz), read with knobly's word reader and readRate
	/// (docs/graph-file.md#rates).
	rate,
	/// JSON true or false; in ear.conf the bare word true or false, read with knobly's boolean reader as disabled's is
	/// (a variable allowed).
	boolean,
};
struct SettingDeclaration {
	std::string_view key;
	SettingKind kind;
};
class AnySettingsTable; // settings_table.hpp
namespace spec_detail {
template <const auto& Spec, class Type> class SpecNodeType; // lifecycle.hpp
} // namespace spec_detail
class CheckedSettings; // settings_table.hpp
/// What a type makes of one node's checked settings: its config, or the whole text of a refusal. parse names the node.
using MadeConfig = std::expected<std::unique_ptr<const NodeConfig>, std::string>;
/// One node type. Its name, its settings' kinds and their checking all answer from the one table makeNodeType
/// (lifecycle.hpp) bound it to, its spec's; none can be overridden, so they cannot disagree.
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
	/// The stores every node of the type has (docs/writing-a-node-type.md#stores), from its spec, in declaration
	/// order: the boot sweep reads them before any node exists.
	std::span<const StoreDeclaration> stores() const noexcept { return stores_; }

private:
	template <const auto& Spec, class Type> friend class spec_detail::SpecNodeType; // nothing else can derive
	explicit NodeType(const AnySettingsTable& table, std::span<const StoreDeclaration> stores = {}) noexcept;
	/// The one customisation point. Called only with what the table's walk made.
	virtual MadeConfig makeConfig(const NodeName& node, CheckedSettings settings) const = 0;
	const AnySettingsTable* table_;            // static storage: the spec's table
	std::span<const StoreDeclaration> stores_; // static storage: the spec's Stores::declared
};
} // namespace ear
