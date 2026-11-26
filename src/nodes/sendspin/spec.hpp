// The "sendspin" node type's spec (docs/node-types.md#sendspin): its banks, its key store, its two loops, and the status
// fields and events its worker writes (docs/node-types.md#status-and-events), ahead of the loops' states (worker.hpp),
// which write them through the Report<spec> configure hands the worker, so the states are only declared here. The class
// is sendspin_spec.hpp's. In the type's own named namespace, never an unnamed one (type_spec.hpp says why).
#pragma once
#include <cstdint>
#include <string_view>

#include "ear/core/lifecycle.hpp"
#include "key_store.hpp"
#include "settings.hpp"

namespace ear::nodes::sendspin {

class Worker;
class Notifier;

inline constexpr auto to = bank::input<"to">;
inline constexpr auto from = bank::output<"from">;
/// The sink's bank: `to` passed straight through, read by ToServer on the sink's thread; never faded.
inline constexpr auto toServer = bank::insideOutput<"to_server">;
/// What the server sends: a source the library's sync thread writes through the node's clock (sendspin_clock.hpp),
/// passed to `from`.
inline constexpr auto fromServer = bank::insideInput<"from_server">;
/// `sendspin-<node>.key` in the graph file folder: mode 0600, at most 64 KiB, created as an empty key record, trusted
/// only as one (docs/node-types.md#the-identity-file).
inline constexpr auto keyFile = store::file<"key">(
	{.mode = 0600, .maxBytes = maxKeyFileBytes, .initial = &emptyKeyRecord, .distrust = &whyNotKeyRecord});
/// The library's main loop: its client, pairing, the time filter, mDNS (`ear-sendspin <node>`).
inline constexpr auto worker = loop::dedicated<"sendspin", Worker>;
/// What played, told to the library apart from the worker (`ear-notify <node>`).
inline constexpr auto notifier = loop::dedicated<"notify", Notifier>;

/// Every field is the worker's: what the library tells it, on its loop (docs/node-types.md#status-and-events).
namespace fields {
inline constexpr std::uint32_t mostOfAPeer = 64; // what is kept of a server's name or id
inline constexpr std::int64_t mostOfAWord = UINT32_MAX;
inline constexpr auto clientId =
	status::text<"clientId">(writer::loop(worker), mostOfAPeer).help("its Sendspin client id, kept in its key file");
inline constexpr std::string_view connections[] = {"dialling", "listening", "connected"};
inline constexpr auto connection = status::choice<"connection">(writer::loop(worker), connections)
                                       .display(Display::box)
                                       .help("dialling or listening until a server is connected");
inline constexpr auto url =
	status::text<"url">(writer::loop(worker), maxServerUrlBytes).help("the server it dials; empty: it listens");
inline constexpr auto listenPort =
	status::number<"listenPort">(writer::loop(worker), 0, 65535, "").help("the port it listens on; 0: it dials");
inline constexpr auto instance =
	status::text<"instance">(writer::loop(worker), 63).help("its mDNS instance name while it listens and advertises");
inline constexpr auto serverName =
	status::text<"serverName">(writer::loop(worker), mostOfAPeer).help("the connected server's name");
inline constexpr auto paired =
	status::flag<"paired">(writer::loop(worker)).help("the connected server is paired: only a paired one gets audio");
inline constexpr auto pairingCode = status::text<"pairingCode">(writer::loop(worker), 16)
                                        .display(Display::callout)
                                        .help("type it into Music Assistant to pair");
inline constexpr auto pairingHeld = status::flag<"pairingHeld">(writer::loop(worker))
                                        .display(Display::callout)
                                        .help("20 wrong codes hold pairing back: restart the node to pair again");
inline constexpr std::string_view codecs[] = {"none", "flac", "pcm", "opus"};
inline constexpr auto stream = status::choice<"stream">(writer::loop(worker), codecs)
                                   .initially("none")
                                   .help("the codec of the stream the server plays to it");
inline constexpr auto streamRate = status::number<"streamRate">(writer::loop(worker), 0, mostOfAWord, "Hz");
inline constexpr auto streamChannels = status::number<"streamChannels">(writer::loop(worker), 0, mostOfAWord, "");
inline constexpr auto streamBits = status::number<"streamBits">(writer::loop(worker), 0, mostOfAWord, "bits");
inline constexpr auto sending =
	status::flag<"sending">(writer::loop(worker)).help("the server asked for what is routed to its inputs");
inline constexpr auto sendingRate = status::number<"sendingRate">(writer::loop(worker), 0, mostOfAWord, "Hz");
} // namespace fields

namespace events {
inline constexpr auto paired =
	event::of<"paired">(EventSeverity::info, "paired with server {server}", arg::text<"server">(fields::mostOfAPeer));
inline constexpr std::string_view pairReasons[] = {"attempt-timeout",      "concurrent-attempt",
                                                   "method-not-supported", "pairing-code-mismatch",
                                                   "user-cancelled",       "unknown"};
inline constexpr auto pairingFailed =
	event::of<"pairing-failed">(EventSeverity::warn, "pairing failed: {reason}", arg::choice<"reason">(pairReasons));
inline constexpr auto keyWriteFailed =
	event::of<"key-write-failed">(EventSeverity::error,
                                  "its key file was not written, so the pairing holds for this "
                                  "run only: {detail}",
                                  arg::text<"detail">());
inline constexpr auto advertiseFailed = event::of<"advertise-failed">(
	EventSeverity::warn, "listening but not advertised over mDNS: {detail}", arg::text<"detail">());
inline constexpr auto renamed =
	event::of<"renamed">(EventSeverity::info, "listed over mDNS as {published}, as another responder holds {asked}",
                         arg::text<"asked">(63), arg::text<"published">(63));
} // namespace events

inline constexpr TypeSpec spec{
	.settings = settingsTable,
	.banks = Banks<to, from, toServer, fromServer>{},
	.stores = Stores<keyFile>{},
	.loops = Loops<worker, notifier>{},
	.status = StatusTable<fields::clientId, fields::connection, fields::url, fields::listenPort, fields::instance,
                          fields::serverName, fields::paired, fields::pairingCode, fields::pairingHeld, fields::stream,
                          fields::streamRate, fields::streamChannels, fields::streamBits, fields::sending,
                          fields::sendingRate>{},
	.events = EventTable<events::paired, events::pairingFailed, events::keyWriteFailed, events::advertiseFailed,
                         events::renamed>{}};

} // namespace ear::nodes::sendspin
