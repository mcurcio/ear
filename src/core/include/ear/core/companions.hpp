// Companions, slice 3b (docs/design/companions.md#8-module-design; this header is their home until the slice is built,
// then the library docs: so far docs/writing-a-node-type.md#engine-companions and docs/engine.md#engine-companions):
// what a type asks for on an inside bank (the engine companions' shapes; the ring companions' shapes are sink.hpp's and
// source.hpp's), the closed list of kinds and their readiness deadlines, a companion as the wire sees it, and the
// engine companions' adapter interface.
//
// Built so far: the shapes and the engine companions' interfaces (docs/engine.md#engine-companions), which NodeHost
// and ear_pipewire's adapters use, and the kinds' readiness bounds, which NodeHost's deadlines use
// (docs/engine.md#readiness-deadlines); the counters' paces and the wire's view are declarations until the slice's
// later steps build them. Pure: nothing but the standard library and ear_core.
#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include "ear/core/graph_spec.hpp"
#include "ear/core/node_type.hpp"
#include "ear/core/rtp_receiver.hpp"
#include "ear/core/sound_card.hpp"
namespace ear {

// ---- What a type asks for (CreateOf::soundCard, CreateOf::rtpReceiver) ----

/// A sound card on an inside bank: playback on an inside output bank, capture on an inside input bank (the direction is
/// the bank's, checked at compile time), one channel per port of the bank, opened at the graph frequency.
struct SoundCardShape {
	CardId card;
};

/// An RTP receiver on an inside input bank, one channel per port of the bank (1-64). Every member is checked by the
/// type's parse; the framework passes none of them on unparsed.
struct RtpReceiverShape {
	RtpAddress address;
	std::uint16_t port; // UDP port, 1-65535
	RtpEncoding encoding;
	std::uint32_t rate;      // the sender's rate, 8000-192000 Hz; resampled to the graph frequency when it differs
	std::uint32_t latencyMs; // the receive buffer, 1-1000; declared upstream of the bank by the framework
};

/// An engine companion's shape, as NodeHost keeps it until the block is ready.
using EngineCompanionShape = std::variant<SoundCardShape, RtpReceiverShape>;

// ---- The kinds ----

/// The closed list (docs/design/companions.md#32-the-kinds-a-closed-list). Engine kinds are PipeWire nodes joined to
/// the block by links; ring kinds are played or tapped by the block itself, in its own cycle.
enum class CompanionKind { cardPlayback, cardCapture, rtpReceiver, sink, source };
/// "card-playback" | "card-capture" | "rtp-receiver" | "sink" | "source": the wire's `kind`, the one home of these.
std::string_view companionKindName(CompanionKind kind) noexcept;
/// Whether the kind is an engine node of its own (cards, the RTP receiver) rather than a ring the block serves.
constexpr bool isEngineKind(CompanionKind kind) noexcept {
	return kind == CompanionKind::cardPlayback || kind == CompanionKind::cardCapture ||
	       kind == CompanionKind::rtpReceiver;
}

/// How long a companion may take to become ready, from its request (D-R12): a card's PCM running and linked, an RTP
/// receiver's node running and linked, a sink's consumer begun. Past it the companion is lost with its kind's reason and
/// the node fails. A source is ready when made and has none.
inline constexpr std::chrono::milliseconds cardReadinessBound{10'000};
inline constexpr std::chrono::milliseconds rtpReceiverReadinessBound{5'000};
inline constexpr std::chrono::milliseconds sinkReadinessBound{5'000};
/// The bound for `kind`; std::nullopt for a source.
std::optional<std::chrono::milliseconds> readinessBound(CompanionKind kind) noexcept;

/// How often a node's companion counters are sampled for the wire (K1), and the least time between two
/// ear.sink-dropped or ear.source-shortfall diagnostics of one companion (K2).
inline constexpr std::chrono::milliseconds companionCounterPace{1'000};
inline constexpr std::chrono::milliseconds companionDropDiagnosticPace{10'000};

// ---- A companion as the wire and the editor see it ----

/// One entry of a node's `companions` (feature `companions`, docs/design/companions.md#37-on-the-wire-and-in-the-editor).
struct CompanionView {
	/// An engine kind's engine node ("<node>.playback", "<node>.capture", "<node>.rtp"); a ring kind's "<node>.<bank>".
	NodeName name;
	CompanionKind kind;
	std::string bank; // the inside bank it joins: inside banks appear nowhere else on the wire
	bool ready = false;
	std::optional<std::uint64_t> dropped{};   // a sink: frames its ring had no room for, this configuration
	std::optional<std::uint64_t> shortfall{}; // a source: frames played as zeros while its writer was not idle
	friend bool operator==(const CompanionView&, const CompanionView&) = default;
};

/// Told whenever a node's list changes: an entry added or removed, its `ready` changed, or (sampled at most once per
/// companionCounterPace) a counter moved. Main thread. A list that shows a companion ready is told before the health
/// change its readiness allowed; a teardown tells an empty list when it begins. `companions` lives only for the call.
class NodeCompanionsListener {
public:
	virtual void companionsChanged(const NodeName& node, std::span<const CompanionView> companions) = 0;

protected:
	~NodeCompanionsListener() = default;
};

// ---- Engine companions (ear_pipewire implements the adapters) ----

/// An engine companion: a PipeWire node a configuration asked for (a sound card or an RTP receiver), plus the links
/// joining its channels to the block's inside ports. Destroying it (main thread) removes both, in any state, and tells
/// nothing.
class EngineCompanion {
public:
	virtual ~EngineCompanion() = default;
};

/// One engine companion's lifecycle, the same for every engine kind (main thread).
class EngineCompanionEvents {
public:
	/// It runs (a card's PCM runs; a receiver's socket is bound) and every channel is linked to its block port; at most
	/// once.
	virtual void ready() = 0;
	/// At most once, before or after ready, never inside make(). Its engine node and links are already destroyed when it
	/// is told, so it no longer clocks or holds any part of the graph. Destroying the EngineCompanion inside lost() is
	/// allowed (not inside ready()): the adapter touches nothing of it after telling (NodeHost tears a failed
	/// configuration down in the same call).
	virtual void lost(std::string detail) = 0;
	/// As lost, when what ended it was ear's own bug in one of its callbacks (docs/architecture.md#faults-by-owner): the
	/// node fails lost with `detail` (internalErrorDetail's) rather than engine-refused. At most once, instead of lost.
	virtual void faulted(std::string detail) { lost(std::move(detail)); }

protected:
	~EngineCompanionEvents() = default;
};

/// Where an engine companion goes, worked out by the framework from the bank: its one engine name, the block it joins
/// (its name, and its engine node's global id, PortBlock::blockId, which its links name:
/// docs/architecture.md#phases-states-and-what-is-wanted), the bank's direction, and the block's ports it joins,
/// channel i to blockPorts[i], and the inside bank's name, which NodeHost keeps for the companion it asked for (an
/// adapter in the engine reads its ports; the type harness's fake reads the name, so it never works a bank out again).
struct CompanionTarget {
	NodeName engineNode;              // "<node>.playback" | "<node>.capture" | "<node>.rtp"
	NodeName blockNode;               // the node's name, its block's
	Direction bank;                   // the inside bank's: output (the companion reads the block), input (it feeds it)
	std::vector<PortName> blockPorts; // the bank's ports
	AudioFormat format;
	std::uint32_t blockId = 0xFFFFFFFFU; // PortBlock::blockId; no id: the join cannot link and the companion is lost
	std::string insideBank{};            // the inside bank's name: one engine companion per bank
};

/// One engine kind's adapter. Each strips nothing it did not set: it never sets node.force-quantum, node.force-rate,
/// node.lock-quantum or node.lock-rate, and a test seam that would is refused
/// (docs/design/companions.md#36-the-quantum-guard).
///  - SoundCardShape: one ALSA adapter for shape.card with blockPorts.size() channels at format.rate, playback when
///    target.bank is output, capture when input. It joins the graph's clock only through its links to the block, made
///    once its PCM runs.
///  - RtpReceiverShape: one libpipewire-module-rtp-source node receiving the stream the shape names, with
///    blockPorts.size() channels. It follows the graph's clock and never drives it; its channels are linked once it
///    runs (its socket is bound). Packets that stop or never come change nothing: it stays ready and its channels are
///    silent.
template <class Shape> class EngineCompanionAdapter {
public:
	/// Main thread; only once port block `target.blockNode` is ready (its ports exist, and target.blockId is its id).
	/// NodeHost calls it inside its per-turn flush (docs/architecture.md#isolation): a ready() told inside it is heard
	/// once it returned.
	virtual std::unique_ptr<EngineCompanion> make(const CompanionTarget& target, const Shape& shape,
	                                              EngineCompanionEvents& events) = 0;

protected:
	~EngineCompanionAdapter() = default;
};
using SoundCardCompanions = EngineCompanionAdapter<SoundCardShape>;
using RtpReceiverCompanions = EngineCompanionAdapter<RtpReceiverShape>;

} // namespace ear
