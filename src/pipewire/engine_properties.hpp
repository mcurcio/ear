// Every PipeWire property ear sets, in engine.conf or in code: one row each, with what it couples to, the
// docs/engine.md heading that says why and the test that pins the coupling (docs/engine.md#properties-ear-sets).
// Private to ear_pipewire: its sources and tests include it by this path; no public header does.
//
// The table is checked four ways: here at compile time (a builder takes each key from property(), which does not
// compile for a key the table does not list for that home, and the static_asserts below); against the live engine
// and engine.conf (engine_properties_test.cpp); each row's pinnedBy against the tests that exist; and against
// docs/engine.md's tables, which must list the same rows.
#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace ear::pipewire {

/// Where ear sets a property: the object that carries it and the code (or file) that builds it.
enum class Home {
	engineConf,    // engine.conf's context.properties
	context,       // contextProperties() in engine.cpp: pw_context_new's own properties
	internalClock, // engine.conf's context.objects: the dummy driver's arguments
	block,         // a port block's pw_filter (port_blocks.cpp)
	blockPort,     // each of a port block's ports (port_blocks.cpp)
	card,          // a sound card's adapter node (sound_cards.cpp)
	cardSeam,      // what AlsaSoundCards' test seam adds to a card: clocklessCardProperties() alone
	rtpReceiver,   // libpipewire-module-rtp-source's arguments (rtp_receivers.cpp)
	rtpStream,     // the receiver's stream.props argument: its node's own properties
	link,          // a route link (links.cpp)
	companionLink, // an engine companion's link to its block (engine_companions.cpp)
};

/// How docs/engine.md's tables name each home.
constexpr std::string_view homeName(Home home) noexcept {
	switch (home) {
	case Home::engineConf:
		return "engine.conf";
	case Home::context:
		return "context";
	case Home::internalClock:
		return "internal clock";
	case Home::block:
		return "block";
	case Home::blockPort:
		return "block port";
	case Home::card:
		return "card";
	case Home::cardSeam:
		return "card seam";
	case Home::rtpReceiver:
		return "RTP receiver";
	case Home::rtpStream:
		return "RTP stream";
	case Home::link:
		return "route link";
	case Home::companionLink:
		return "companion link";
	}
	return {};
}

inline constexpr std::array allHomes{Home::engineConf, Home::context, Home::internalClock, Home::block,
                                     Home::blockPort,  Home::card,    Home::cardSeam,      Home::rtpReceiver,
                                     Home::rtpStream,  Home::link,    Home::companionLink};

struct PropertyRow {
	Home home;
	std::string_view key;
	std::string_view value;     // as set, or how it is computed ("the block size")
	std::string_view couplesTo; // what else it changes, in PipeWire's terms
	std::string_view anchor;    // the docs/engine.md heading that says why
	std::string_view pinnedBy;  // the Catch test name that pins the coupling
};

namespace pinned {
// The tests rows name, once each. engine_properties_test.cpp fails for a name no test has.
inline constexpr std::string_view live = "Every property ear sets is held by PipeWire as the property table says";
inline constexpr std::string_view noDaemon =
	"The engine runs with no daemon, runtime directory or D-Bus, and an unlinked sink block processes zeros";
inline constexpr std::string_view oneDataLoop = "S3: the engine has one data loop, context.num-data-loops = 1, and "
												"blocks, a sound card and an RTP receiver all run on it";
inline constexpr std::string_view rateAndBlock = "Blocks process at the graph frequency and block size";
inline constexpr std::string_view notPowerOfTwo = "A block size that is not a power of two runs as itself";
inline constexpr std::string_view quantumGuard = "The quantum floor and limit are the block size: a node forcing a "
												 "larger or a smaller quantum leaves every cycle at the block size";
inline constexpr std::string_view group =
	"Every block is in the ear.graph group and processes on the dummy driver alone";
inline constexpr std::string_view bothClocks =
	"Each cycle carries its start on both clocks, the internal clock as its driver, its rate and the next cycle's "
	"start, and only the first is a discontinuity";
inline constexpr std::string_view stalls = "After data-thread stalls past 10 ms the internal clock catches up in whole "
										   "blocks, never steps back, and keeps its phase to the raw clock";
inline constexpr std::string_view livePorts = "Live ports name their node, port and direction";
inline constexpr std::string_view linkCarries =
	"A link carries a source block's ramp to a sink block sample for sample";
inline constexpr std::string_view cardFollows = "A playback card becomes ready, every channel's link active, and then "
												"follows: the internal clock drives it and every port block";
inline constexpr std::string_view cardAlone =
	"A card running alone holds no port block: they stay on the dummy until it is ready";
inline constexpr std::string_view cardJoins =
	"A card joins its block by the block's id: beside an older block of the same name it reaches only the newer";
inline constexpr std::string_view cardChannels =
	"What a port block sends to inside output to_card.2 lands on the card's channel 2";
inline constexpr std::string_view cardCannotOpen =
	"A card that cannot open is lost after make returns, its node already gone, and the blocks keep running";
inline constexpr std::string_view cardBuffer =
	"A card's engine node carries no property that forces or locks the graph's quantum or rate, keeps the adapters' "
	"old quantum limit for its ALSA buffer, and the extra properties refuse every clock property";
inline constexpr std::string_view receiverReady =
	"A receiver is ready once, every channel's link active, and it and the block follow the Dummy-Driver";
inline constexpr std::string_view receiverJoins = "A receiver joins its block by the block's id: beside an older block "
												  "of the same name it reaches only the newer";
inline constexpr std::string_view receiverChannels =
	"L24 at the graph rate: channel 1 reaches from_rtp.1 and channel 2 from_rtp.2";
inline constexpr std::string_view receiverResamples = "L16 sent at 44100 into the 48000 graph is resampled";
inline constexpr std::string_view receiverSsrc = "A sender that restarts with a new SSRC is heard again";
inline constexpr std::string_view receiverBuffer =
	"A real receiver's buffer beyond from_rtp reaches stream whole: the told capture latency's least and most are "
	"both the buffer once the receiver is linked and reports its own";
} // namespace pinned

/// One row per key ear sets. The reasons are docs/engine.md's (its table of these rows, and the heading each names).
inline constexpr std::array engineProperties = std::to_array<PropertyRow>({
	// engine.conf, context.properties.
	{Home::engineConf, "core.daemon", "false", "no socket and no runtime directory", "properties-ear-sets",
     pinned::noDaemon},
	{Home::engineConf, "core.name", "ear", "nothing", "properties-ear-sets", pinned::live},
	{Home::engineConf, "support.dbus", "false", "no D-Bus connection", "properties-ear-sets", pinned::noDaemon},
	{Home::engineConf, "context.num-data-loops", "1",
     "one data loop runs every engine node, so one clock.position a cycle and invokes between cycles",
     "the-one-data-loop-and-invokes", pinned::oneDataLoop},
	// contextProperties(): the clock, from EngineSettings.
	{Home::context, "config.name", "the engine.conf path EngineSettings names",
     "the file pw_context_new reads; its context.properties overwrite these", "properties-ear-sets", pinned::live},
	{Home::context, "default.clock.rate", "the graph frequency", "clock.rate.denom every block sees", "the-clock",
     pinned::rateAndBlock},
	{Home::context, "default.clock.allowed-rates", "[ the graph frequency ]", "the graph never switches rate",
     "the-clock", pinned::rateAndBlock},
	{Home::context, "default.clock.quantum", "the block size", "clock.duration every block sees", "the-clock",
     pinned::rateAndBlock},
	{Home::context, "default.clock.min-quantum", "the block size", "the least quantum the graph picks", "the-clock",
     pinned::rateAndBlock},
	{Home::context, "default.clock.max-quantum", "the block size", "the most quantum the graph picks", "the-clock",
     pinned::rateAndBlock},
	{Home::context, "clock.power-of-two-quantum", "false", "the quantum is not rounded down to a power of two",
     "the-clock", pinned::notPowerOfTwo},
	{Home::context, "default.clock.quantum-floor", "the block size", "the least a forced quantum is clamped to",
     "the-quantum-guard", pinned::quantumGuard},
	{Home::context, "default.clock.quantum-limit", "the block size",
     "the most a forced quantum is clamped to; module-adapter's clock.quantum-limit, which sizes an ALSA buffer "
     "without a period count",
     "the-quantum-guard", pinned::quantumGuard},
	// engine.conf, context.objects: the internal clock.
	{Home::internalClock, "factory.name", "support.node.driver", "a driver on a timer, with no device",
     "the-internal-clock", pinned::group},
	{Home::internalClock, "node.name", "Dummy-Driver", "nothing", "the-internal-clock", pinned::group},
	{Home::internalClock, "priority.driver", "200000",
     "PipeWire walks drivers in its order: above a card's, so a linked card follows", "the-internal-clock",
     pinned::cardFollows},
	{Home::internalClock, "node.group", "ear.graph", "the blocks' group, so this driver drives them all",
     "the-internal-clock", pinned::group},
	{Home::internalClock, "clock.id", "monotonic-raw",
     "tracking mode on CLOCK_MONOTONIC_RAW; clock.name clock.system.monotonic-raw", "the-internal-clock",
     pinned::bothClocks},
	{Home::internalClock, "resync.ms", "1000000000", "the driver never resets after a stall", "the-internal-clock",
     pinned::stalls},
	// A port block's filter and its ports.
	{Home::block, "node.name", "the node's name", "how the registry, the live graph and link-factory name the block",
     "port-blocks", pinned::livePorts},
	{Home::block, "media.type", "Audio", "nothing", "port-blocks", pinned::live},
	{Home::block, "node.group", "ear.graph", "the internal clock's group, so it drives every block",
     "the-internal-clock", pinned::group},
	{Home::block, "node.always-process", "true", "an unlinked block still runs, on the first active driver",
     "port-blocks", pinned::noDaemon},
	{Home::blockPort, "format.dsp", "32 bit float mono audio", "a DSP port of one float channel", "port-blocks",
     pinned::linkCarries},
	{Home::blockPort, "port.name", "the port's name", "how the live graph and companion links name the port",
     "port-blocks", pinned::livePorts},
	// A sound card's adapter.
	{Home::card, "factory.name", "api.alsa.pcm.sink or api.alsa.pcm.source",
     "the ALSA PCM node module-adapter wraps in a converter", "sound-cards", pinned::cardFollows},
	{Home::card, "api.alsa.path", "hw:CARD=<card id>,DEV=0", "device 0 of the card", "sound-cards",
     pinned::cardCannotOpen},
	{Home::card, "audio.channels", "the bank's channel count", "one adapter port per channel", "sound-cards",
     pinned::cardChannels},
	{Home::card, "audio.rate", "the graph frequency", "the rate the PCM opens at", "sound-cards", pinned::cardFollows},
	{Home::card, "audio.position", "[ AUX0 AUX1 ... ]", "the ports playback_AUX<n> and capture_AUX<n>", "sound-cards",
     pinned::cardChannels},
	{Home::card, "node.name", "<node>.playback or <node>.capture", "how the lifecycle names its links' node",
     "engine-companions", pinned::cardJoins},
	{Home::card, "node.always-process", "true", "it runs alone before it is linked, so RUNNING means its PCM runs",
     "engine-companions", pinned::cardAlone},
	{Home::card, "priority.driver", "1", "below the internal clock's 200000, so linked it follows",
     "the-internal-clock", pinned::cardFollows},
	{Home::card, "api.alsa.period-size", "the block size", "with api.alsa.period-num, the ALSA buffer",
     "the-quantum-guard", pinned::cardBuffer},
	{Home::card, "api.alsa.period-num", "cardPeriods(the block size)", "the ALSA buffer, not 4 x clock.quantum-limit",
     "the-quantum-guard", pinned::cardBuffer},
	{Home::cardSeam, "clock.name", "clock.system.monotonic-raw",
     "a follower whose clock.name is its driver's turns rate matching off", "sound-cards", pinned::cardChannels},
	// An RTP receiver's module arguments.
	{Home::rtpReceiver, "remote.name", "internal", "the module's connection is to ear's own context", "rtp-receivers",
     pinned::receiverReady},
	{Home::rtpReceiver, "source.ip", "the rtp node's address", "the address the socket binds or connects to",
     "rtp-receivers", pinned::receiverChannels},
	{Home::rtpReceiver, "source.port", "the rtp node's port", "the UDP port the socket binds", "rtp-receivers",
     pinned::receiverChannels},
	{Home::rtpReceiver, "sess.media", "audio", "nothing", "rtp-receivers", pinned::receiverChannels},
	{Home::rtpReceiver, "sess.latency.msec", "the rtp node's latencyMs",
     "the ring's target fill, rounded down to whole packets", "latency-per-bank-pair", pinned::receiverBuffer},
	{Home::rtpReceiver, "sess.ignore-ssrc", "true", "no lock on the first sender's SSRC", "rtp-receivers",
     pinned::receiverSsrc},
	{Home::rtpReceiver, "audio.format", "S16BE or S24BE", "the payload's sample format", "rtp-receivers",
     pinned::receiverResamples},
	{Home::rtpReceiver, "audio.rate", "the rtp node's rate", "the receiver resamples it to the graph's",
     "rtp-receivers", pinned::receiverResamples},
	{Home::rtpReceiver, "audio.channels", "the bank's channel count", "one port per channel", "rtp-receivers",
     pinned::receiverChannels},
	{Home::rtpReceiver, "audio.position", "[ AUX0 AUX1 ... ]", "the ports receive_AUX<n>", "rtp-receivers",
     pinned::receiverChannels},
	{Home::rtpReceiver, "node.name", "<node>.rtp", "how ear finds the module's node in the registry", "rtp-receivers",
     pinned::receiverReady},
	{Home::rtpReceiver, "node.description", "<node>.rtp", "link-factory matches descriptions too", "rtp-receivers",
     pinned::receiverJoins},
	{Home::rtpReceiver, "media.name", "<node>.rtp", "nothing", "rtp-receivers", pinned::receiverJoins},
	{Home::rtpReceiver, "stream.props", "the RTP stream rows", "the receiver node's own properties", "rtp-receivers",
     pinned::receiverReady},
	{Home::rtpStream, "node.always-process", "true",
     "it runs before it is linked, so RUNNING means its socket is bound", "rtp-receivers", pinned::receiverReady},
	{Home::rtpStream, "adapter.auto-port-config", "{ mode = dsp monitor = false control = false position = preserve }",
     "the stream's converter makes its ports", "rtp-receivers", pinned::receiverChannels},
	// Links.
	{Home::link, "link.output.port", "the output port's global id", "link-factory finds the port by id alone",
     "no-session-manager", pinned::linkCarries},
	{Home::link, "link.input.port", "the input port's global id", "link-factory finds the port by id alone",
     "no-session-manager", pinned::linkCarries},
	{Home::companionLink, "link.output.node", "the block's or the companion's global id",
     "never an older node of the same name", "engine-companions", pinned::cardJoins},
	{Home::companionLink, "link.output.port", "the output port's name", "a port of that node, by name",
     "engine-companions", pinned::cardChannels},
	{Home::companionLink, "link.input.node", "the companion's or the block's global id",
     "never an older node of the same name", "engine-companions", pinned::receiverJoins},
	{Home::companionLink, "link.input.port", "the input port's name", "a port of that node, by name",
     "engine-companions", pinned::receiverChannels},
});

/// What no object of a home may carry from ear: a key, or every key under a prefix ending in '.'.
inline constexpr auto forbidden = [] {
	constexpr std::array clockForcing{std::string_view("node.force-quantum"), std::string_view("node.force-rate"),
	                                  std::string_view("node.lock-quantum"), std::string_view("node.lock-rate")};
	constexpr std::array nodes{Home::internalClock, Home::block,       Home::card,
	                           Home::cardSeam,      Home::rtpReceiver, Home::rtpStream};
	std::array<std::pair<Home, std::string_view>, clockForcing.size() * nodes.size() + 6> all{};
	std::size_t at = 0;
	for (const Home home : nodes)
		for (const std::string_view key : clockForcing) all[at++] = {home, key};
	all[at++] = {Home::engineConf, "default.clock."};
	all[at++] = {Home::engineConf, "clock.power-of-two-quantum"};
	all[at++] = {Home::engineConf, "cpu.zero.denormals"};
	all[at++] = {Home::card, "node.group"};
	all[at++] = {Home::card, "node.driver"};
	all[at++] = {Home::card, "node.link-group"};
	return all;
}();

/// Whether the table lists `key` for `home`.
constexpr bool listed(Home home, std::string_view key) noexcept {
	for (const PropertyRow& row : engineProperties)
		if (row.home == home && row.key == key) return true;
	return false;
}

/// Whether `forbidden` refuses `key` on `home`.
constexpr bool isForbidden(Home home, std::string_view key) noexcept {
	for (const auto& [where, refused] : forbidden) {
		if (where != home) continue;
		if (refused.ends_with('.') ? key.starts_with(refused) : key == refused) return true;
	}
	return false;
}

/// What AlsaSoundCards' test seam may not add to a card: a key forbidden there, or one ear itself sets on every card.
constexpr bool refusedOnCardSeam(std::string_view key) noexcept {
	return isForbidden(Home::card, key) || listed(Home::card, key);
}

namespace detail {
// Called only where property() finds no row: not a constant expression, so the call does not compile.
inline const char* notInThePropertyTable() { return nullptr; }
} // namespace detail

/// The key, if the table lists it for `home`; does not compile otherwise.
consteval const char* property(Home home, std::string_view key) {
	for (const PropertyRow& row : engineProperties)
		if (row.home == home && row.key == key) return row.key.data();
	return detail::notInThePropertyTable();
}

namespace detail {
constexpr bool rowsUnique() {
	for (std::size_t i = 0; i < engineProperties.size(); ++i)
		for (std::size_t j = i + 1; j < engineProperties.size(); ++j)
			if (engineProperties[i].home == engineProperties[j].home &&
			    engineProperties[i].key == engineProperties[j].key)
				return false;
	return true;
}
constexpr bool noRowForbidden() {
	for (const PropertyRow& row : engineProperties)
		if (isForbidden(row.home, row.key)) return false;
	return true;
}
// engine.conf's context.properties overwrite pw_context_new's own (context.c): no key may be in both.
constexpr bool confLeavesTheContextAlone() {
	for (const PropertyRow& row : engineProperties)
		if (row.home == Home::context && listed(Home::engineConf, row.key)) return false;
	return true;
}
constexpr bool seamRowsAllowed() {
	for (const PropertyRow& row : engineProperties)
		if (row.home == Home::cardSeam && refusedOnCardSeam(row.key)) return false;
	return true;
}
constexpr bool rowsComplete() {
	for (const PropertyRow& row : engineProperties)
		if (row.key.empty() || row.value.empty() || row.couplesTo.empty() || row.anchor.empty() || row.pinnedBy.empty())
			return false;
	return true;
}
} // namespace detail

static_assert(detail::rowsUnique(), "a home lists a key twice");
static_assert(detail::noRowForbidden(), "a row sets a key forbidden on its home");
static_assert(detail::confLeavesTheContextAlone(), "engine.conf would overwrite a property Engine sets");
static_assert(detail::seamRowsAllowed(), "the card seam would set a key the card sets or may not carry");
static_assert(detail::rowsComplete(), "every row names its value, coupling, heading and test");

} // namespace ear::pipewire
