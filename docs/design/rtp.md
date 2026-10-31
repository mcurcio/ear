# The `rtp` node: FPP's audio into ear over RTP

Status: direction approved by the owner at the gate (https://claude.ai/artifact/5qGsXixMQmaS6ZsoGNgZ5v,
2026-10-30). This page is the short record of that gate, the spike's results and the frozen
pages of the tracer milestone (slices R1a, R1b, R1n and R1c), written by the code architect on
2026-10-30. The milestone is the thinnest real path, kept: an `rtp` node in a graph file hears a
tone sent over 127.0.0.1 and a `file` node records it. Then the work stops for the owner
(section 9 lists what waits). Layer: module, inside the agreed system design
(`docs/design/pipewire-graph.md`) and batch 2's module map (`docs/design/batch2.md`).

[V] = verified in PipeWire 1.4.2's sources (`/Volumes/Dock/tmp/ear-s1-pwsrc/pipewire-1.4.2`,
the version in the build image) or by the spike; [U] = unverified, so an acceptance criterion
checks it.

## 1. The owner's words

He runs FPP (Falcon Player) in his Kubernetes cluster and wants its audio as a source in ear.
What FPP sends on the network is not known.

| # | His words | Date, where | What it means here |
|---|---|---|---|
| R1 | "I don't know. Can't we make it configurable?" | 2026-10-30, chat, asked what FPP sends (format, rate, channels, address) | The stream's address, port, format, rate and channel count are node settings |
| R2 | "If 100ms is the best rep can do, then so be it" | 2026-10-30, chat | About 100 ms of latency is accepted for this source |
| R3 | "Settings" | 2026-10-30, chat, asked: settings or SAP discovery | Each node names its stream in its settings; no discovery |
| R4 | "in a general purpose sources-and-sinks graph, its always just 'running'" | 2026-10-29, chat | The graph keeps running whatever a source does |
| R5 | he "didnt want to assume that a single hardware device was driving the entirety of the graph" | 2026-10-29, chat | A network source follows the graph's clock and never drives it |
| R6 | "sources and sinks that have a configurable frequency should be proactively configured to match the 'graph frequency'" | 2026-10-29, chat | The rate defaults to the graph frequency; ear cannot set FPP's rate, so a sender at another rate is resampled |

Gate answers (2026-10-30, on the gate page):

| Question | His answer | What it means |
|---|---|---|
| Q1: with no packets, the node stays `running` and silent (the meters show it) | "Agreed" | No "receiving" status in this milestone |
| Q2: how far the first run goes | "Thin path, then stop" (as the workflow brief quotes it; the session's memory notes "Tracer", meaning being confirmed) | One PR with the thinnest real end-to-end path, then the owner looks |

## 2. Current choices (revisable; none is his requirement)

- A new node type `rtp` receives one stream with PipeWire 1.4.2's
  `libpipewire-module-rtp-source`, loaded inside ear's own engine (no daemon, no session
  manager), joined channel for channel to the node's port block as the `card` node's ALSA adapter
  is.
- Settings: `address`, `port`, `format` (`L16` or `L24`), `channels`, `rate` (default: the graph
  frequency), `latencyMs` (default 100). Section 3 has the grammar.
- `address` is `0.0.0.0` (any sender) or an IPv4 multicast group. A unicast address is refused:
  the spike showed what PipeWire makes of one (section 4, finding e2). This narrows the draft's
  "an IP literal".
- One lifecycle for every outside node (section 6, rule 1). Contracts stay plain named methods:
  `NodeGrant::soundCard` stays, `NodeGrant::rtpReceiver` is added.
- No user text reaches PipeWire unparsed: the address has a closed grammar, the format is an
  enum, every number is ranged.
- Deferred: SAP discovery, Opus, AES67's PTP mode (`sess.ts-direct`), an RTP sink, a
  "receiving" status, IPv6, a check of a port that cannot be bound (section 9). Not planned:
  RTSP (PipeWire receives none).

## 3. Settings, ports and names

```json
{ "name": "fpp", "type": "rtp",
  "settings": { "address": "239.69.0.1", "port": 5004, "format": "L24", "channels": 2,
                "rate": 48000, "latencyMs": 100 } }
```

| Key | Required | Grammar | Refused with (detail of `invalid-settings`) |
|---|---|---|---|
| (settings) | yes | a JSON object | `settings: must be an object with "address", "port", "format" and "channels"` |
| any other key | | none | `unknown key <key>` |
| `address` | yes | `RtpAddress`: `0.0.0.0`, or 224.0.0.0-239.255.255.255; four decimal parts 0-255, no leading zeros, nothing else | `address: missing` / `address: must be "0.0.0.0" or an IPv4 multicast group (224.0.0.0-239.255.255.255), as a dotted quad` |
| `port` | yes | whole number 1-65535 | `port: missing` / `port: must be a whole number, 1-65535` |
| `format` | yes | `"L16"` or `"L24"` | `format: missing` / `format: must be "L16" or "L24"` |
| `channels` | yes | whole number 1-64 | `channels: missing` / `channels: must be a whole number, 1-64` |
| `rate` | no | whole number 8000-192000 (Hz); absent: the graph frequency, taken at start from `NodeGrant::format()` | `rate: must be a whole number of Hz, 8000-192000` |
| `latencyMs` | no | whole number 1-1000; absent: 100 (this parser is the one home of that default; ear always passes it on) | `latencyMs: must be a whole number of milliseconds, 1-1000` |

Checks run in this order and the first failure is told: object, unknown keys (in the object's
order), address, port, format, channels, rate, latencyMs.

- Ports: outputs `channel_1`..`channel_N`, inside inputs `from_rtp_1`..`from_rtp_N` (N =
  `channels`). Audio passes straight through, `from_rtp_i` to `channel_i`.
- Engine node names: the port block is `<node>`; the receiver is `<node>.rtp` (one home:
  `NodeHost`'s naming, beside `<node>.playback` and `<node>.capture`). A node name never holds
  `.`, so neither collides with a node.
- The receiver's ports in PipeWire: `receive_AUX0`..`receive_AUX<N-1>` (port.name of a
  `node.network` stream with AUX positions, [V] `impl-port.c:1245-1306` and the spike).

## 4. Spike (2026-10-30): the premises, refuted or not

A throwaway C++ program (not committed) ran PipeWire 1.4.2 in-process exactly as ear does:
`src/pipewire/engine.conf`, the graph clock properties of `engine.cpp`, `pw_context_connect_self`,
no daemon, no runtime directory, no D-Bus. A `pw_filter` block in group `ear.graph` (always
processing, F32 mono inputs) recorded what it got. The module was loaded with
`pw_context_load_module`; the receiver's ports were linked to the block through `link-factory` by
name; a sender thread wrote RTP (version 2, payload type 96, one SSRC, 48 frames a packet, paced
at its own rate on the steady clock) to 127.0.0.1. Sample content was checked by a least-squares
sine fit per channel: channel 1 750 Hz, channel 2 1500 Hz, amplitude 0.5.

| Premise | Result | Evidence |
|---|---|---|
| (a) it loads in-process | **True, with `remote.name = internal`.** `pw_context_get_object(context, Core)` is null after `pw_context_connect_self` (nothing registers the core), so the module calls `pw_context_connect` with its `remote.name` argument; unset, that is the daemon socket, which ear does not serve. `internal` is protocol-native's connection to the local context. | `module-rtp-source.c:671-679`, `module-protocol-native.c:1336-1350`; spike: `pw_context_get_object(Core) = (nil)`, load returned the module, its node appeared |
| (b) one port per channel, nameable | **True, with `stream.props = { adapter.auto-port-config = { mode = dsp ... } }`.** Without it the stream's converter is in dsp mode with no format, so it has **no ports at all** (the card's trap, in another form). With it: `receive_AUX0`..`receive_AUX7` for 8 channels. (For the card's ALSA adapter the same key failed; for a stream, whose format is fixed, it works.) | `audioadapter.c:2164-2171`, `audioconvert.c:1606-1617`; spike runs "noauto" (node, no port, no link) and 2 and 8 channels |
| (c) follows the graph clock, never drives | **True.** The receiver (a `pw_stream` without the driver flag, `node.want-driver = true`) and the block both report `node.driver-id` = the Dummy-Driver's id, read by binding afresh. | spike: `node.driver-id=11 (dummy=11)` for both, every run |
| (d) another sender rate is resampled | **True.** A sender at 44100 into a 48000 graph fits 750.0 and 1500.0 Hz (unresampled would read 816 and 1633 Hz); a 48000 sender into a 44100 graph too. The receiver's own rate adjustment (`pw_stream_set_rate` from its delay-locked loop) keeps its buffer at the target. | spike: zero-crossing frequency 750.0/1500.0 Hz, fit amplitude 0.4990-0.4998, residual 0.010-0.045 |
| (e) receiving from any address over 127.0.0.1 | **True for `source.ip = 0.0.0.0`** (and `::`). The sender was the test's own thread, not `module-rtp-sink` (NOT RUN: an rtp-sink sender). | spike runs at 48000 and 44100 |
| (e2) a unicast `source.ip` | **Not "this host's address".** PipeWire binds any address and `connect()`s to `source.ip:source.port`, so it hears only that sender, and only from **source port == the receiving port**. A sender with its own source port is silent. | `module-rtp-source.c:277-283, 314-319`; spike: `127.0.0.1` with an ephemeral sender port silent, with sender port 46010 heard; `10.9.9.9` silent |
| (f) L16 and L24 | **True** for S16BE and S24BE; 2 and 8 channels. | spike: `rtp.mime=L24`, fits as above |
| (g) a port that cannot be bound | **No event reaches ear.** The module opens its socket when its stream starts (`STREAMING`); a failed bind sets the stream's error, which is only logged: the node is not set to ERROR, not removed, the module not destroyed. Links stay active and the output is silent. With `node.always-process = true` the node starts alone before it is linked, and a failed bind leaves it **never RUNNING**; a bound one reports RUNNING. | `module-rtp-source.c:369-386`, `module-rtp/stream.c:228-251`, `client-node.c:1254-1262`; spike "occupy" runs: `bind() failed: Address already in use`, node stays `suspended`, no destroy event |
| (h) packets stop | **The module never destroys itself** (the destroy call is commented out). After `cleanup.sec` (default 60 s) without packets it sets the node property `rtp.receiving = false` (true again on the next packet); a bound node proxy sees it, the registry does not. The node stays running. | `module-rtp-source.c:456-481`; spike with `cleanup.sec = 1`: `rtp.receiving=false` at 3 s, state running |

Further findings that change what ear passes or allows:

1. **SSRC lock.** The receiver locks onto the first packet's SSRC and drops every other one for
   good, so a sender that restarts with a new SSRC (FPP restarting) is never heard again. With
   `sess.ignore-ssrc = true` audio resumes. [V] `module-rtp/audio.c:131-136, 199-206`; spike:
   default run silent after the SSRC change (1 warning per packet), ignore-ssrc run heard
   throughout.
2. **A host name blocks the main loop.** `source.ip = fpp.local` took 15 s to refuse (a
   blocking name lookup in `pw_net_parse_address`), then the load failed with EINVAL. Hence the
   closed grammar: no name ever reaches it.
3. **Datagram size.** The receiver reads into a buffer of `net.mtu` bytes (default 1280); a
   longer datagram is cut short and its samples garbled. 1 ms packets at 48 kHz fit up to 8
   channels of L24 or 13 of L16. [V] `module-rtp-source.c:715-716`. Not changed in this
   milestone (section 9).
4. **Latency rounding.** `sess.latency.msec` is rounded down to a whole number of packet times
   computed from the MTU (100 ms became 96.9 ms for 2-channel L16). Measured end to end: the
   first sample reached the block about 95 ms after the first packet was sent.
5. **Ring size.** The receiver's ring holds 4 MiB, so 64 channels of L24 hold about 455 ms at
   48 kHz; a `latencyMs` near that would overrun. Not checked by the parser (section 9).

## 5. What ear passes PipeWire

`RtpReceivers` (ear_pipewire) builds the module's argument string in one place, from the request
and the engine node's name only. Every value is a number, a closed enum or an `RtpAddress`; the
engine node name is `<node>.rtp`, whose characters are `[A-Za-z0-9_.-]`.

```
{ remote.name = "internal"
  source.ip = "<address>"  source.port = <port>
  sess.media = "audio"  sess.latency.msec = <latencyMs>  sess.ignore-ssrc = true
  audio.format = "S16BE" | "S24BE"  audio.rate = <rate>  audio.channels = <N>
  audio.position = [ AUX0 AUX1 ... AUX<N-1> ]
  node.name = "<engineNode>"  node.description = "<engineNode>"  media.name = "<engineNode>"
  stream.props = { node.always-process = true
                   adapter.auto-port-config = { mode = dsp monitor = false control = false position = preserve } } }
```

- `remote.name = internal`: finding (a). `adapter.auto-port-config`: finding (b).
- `node.always-process = true` (in `stream.props`, where the stream reads it): the node runs on
  the graph's driver before it is linked, so RUNNING proves its socket is bound (finding g) and
  the lifecycle waits for that event as it waits for a card's PCM.
- `node.description` and `media.name` replace the host name the module would use. `link-factory`
  matches a node's name against nick and description too (the `pipewire-engine` skill's trap).
  The module copies `node.name`, `node.description` and `media.name` from the top level into the
  stream when `stream.props` lacks them [V] `module-rtp-source.c:603-617` (the spike also set the
  first two inside `stream.props`; that duplicate is dropped).
- Left at PipeWire's defaults: `cleanup.sec` (60), `net.mtu` (1280), `sess.min-ptime` and
  `max-ptime`, `local.ifname` (none: a multicast group is joined on the default interface).
- `engine.conf` needs no change: the spike loaded the module with ear's file as it is; Debian's
  `libpipewire-0.3-modules`, already in the dev and run images, holds it.

## 6. Design rules

1. **One outside-node lifecycle.** An outside node is a PipeWire node a node's insides ask ear for,
   joined channel for channel to the node's inside ports and torn down with the insides. Sound
   cards are one kind, RTP receivers the second. Every kind is made, joined, made ready, lost and
   torn down by the same code: `NodeHost` in ear_core (`OutsideNode`, `OutsideNodeEvents`) and
   `joinOutsideNode` in ear_pipewire. A kind says only how its PipeWire node is made, how ear hears
   from it (its own proxy, or the registry), how its ports are configured, and what makes it
   fail. No second copy of the card's ready, lost or link code.
2. **Network sources follow the graph's clock, never drive it** (R5).
3. **No user text reaches PipeWire unparsed** (the `CardId` rule, for RTP: `RtpAddress`).
4. **An outside node's links name it by global id**, never by node.name: a node of the same name
   that is still going (a restart's old receiver, removed through another connection) would
   otherwise match first. The block side stays named by node.name (an ear node name, unique).

## 7. Frozen interfaces

Committed as compiling code with this page (interface stage): the headers below, the renames in
every user, `NodeGrant::rtpReceiver` in every grant (the tests' fakes throw), `NodeHost`'s new
parameter (the tests pass a factory that fails if used), and two stubs that throw
`std::logic_error` naming the slice that replaces them (`NodeHost`'s grant `rtpReceiver`,
`RtpReceivers::make`). No node type asks for a receiver yet, so ear behaves as before. The full
text of each header is in the file; the signatures that matter are repeated here.

### 7.1 ear_core

`src/core/include/ear/core/rtp_receiver.hpp` (new):

```cpp
class RtpAddress {
public:
  static std::optional<RtpAddress> parse(std::string_view text);   // nullopt outside the grammar
  const std::string& text() const noexcept;                        // as parsed (already canonical)
  bool multicast() const noexcept;                                 // false only for "0.0.0.0"
  friend bool operator==(const RtpAddress&, const RtpAddress&) = default;
private:
  explicit RtpAddress(std::string text);
  std::string text_;
};
enum class RtpEncoding { l16, l24 };
struct RtpReceiverRequest {
  RtpAddress address;
  std::uint16_t port;                 // 1-65535
  RtpEncoding encoding;
  std::uint32_t rate;                 // the sender's rate, 8000-192000 Hz
  std::uint32_t latencyMs;            // 1-1000
  std::vector<PortName> blockPorts;   // channel i <-> blockPorts[i] (insideInputs); 1-64 of them
};
```

`node_type.hpp` (changed): `NodeGrant` gains

```cpp
  /// Main thread, inside start() only, at most once per start (else std::logic_error). The
  /// framework receives the stream once the port block is ready and joins its channels to
  /// request.blockPorts; readiness, loss and teardown as soundCard's.
  virtual void rtpReceiver(const RtpReceiverRequest& request) = 0;
```

`node_host.hpp` (changed): `SoundCard` is now `OutsideNode`, `SoundCardEvents` is now
`OutsideNodeEvents` (same `ready()` and `lost(std::string)`; `lost` may come before or after
`ready`); `SoundCardFactory::make` returns `std::unique_ptr<OutsideNode>` and takes
`OutsideNodeEvents&`; new

```cpp
class RtpReceiverFactory {
public:
  virtual std::unique_ptr<OutsideNode> make(const NodeName& engineNode, const NodeName& blockNode,
      const RtpReceiverRequest& request, OutsideNodeEvents& events) = 0;
protected:
  ~RtpReceiverFactory() = default;
};
NodeHost(AudioFormat format, PortBlockFactory& blocks, HandoffFactory& handoffs,
         SoundCardFactory& cards, RtpReceiverFactory& receivers);
```

Invariants (R1a builds them; the card's are unchanged): the outside nodes a start asked for are
made once the block is ready, in request order, named `<node>.playback`, `<node>.capture` and
`<node>.rtp` by one naming function; the insides are ready when every outside node is ready; a
loss before or after that fails the node `engine-refused: <detail>`; on restart, removal and stop
the outside nodes are destroyed before the handoffs close and the insides go.

### 7.2 ear_pipewire

`src/pipewire/include/ear/pipewire/rtp_receivers.hpp` (new, public):

```cpp
class RtpReceivers final : public RtpReceiverFactory {
public:
  explicit RtpReceivers(Engine& engine);   // engine outlives it
  std::unique_ptr<OutsideNode> make(const NodeName& engineNode, const NodeName& blockNode,
      const RtpReceiverRequest& request, OutsideNodeEvents& events) override;
private:
  Engine& engine_;
};
```

`src/pipewire/outside_nodes.hpp` (new, private to the package):

```cpp
enum class ChannelFlow { blockToNode, nodeToBlock };
struct OutsideJoin {
  std::string kind;                   // "card", "RTP receiver": names it in loss details
  NodeName blockNode;
  ChannelFlow flow;
  std::vector<PortName> nodePorts;    // channel i's port on the outside node
  std::vector<PortName> blockPorts;   // channel i's inside port on the block; same size
};
class OutsideBodyEvents {
public:
  virtual void running(std::uint32_t nodeId) = 0;   // RUNNING; the node's global id
  virtual void failed(std::string detail) = 0;      // may be told inside the body's constructor
protected:
  ~OutsideBodyEvents() = default;
};
class OutsideBody { public: virtual ~OutsideBody() = default; };   // destroying removes the node
using OutsideBodyMaker = std::move_only_function<std::unique_ptr<OutsideBody>(OutsideBodyEvents&)>;
std::unique_ptr<OutsideNode> joinOutsideNode(Engine& engine, OutsideJoin join,
    OutsideNodeEvents& events, OutsideBodyMaker makeBody);
```

The lifecycle (batch 2's card state machine, moved, not rewritten): opening, then on `running(id)`
one link per channel through `link-factory` (the outside node by id, the block by name, ports by
name, `LinkProxy` from `link_proxy.hpp`), ready once every link is ACTIVE; on `failed`, a link
failed or gone: silence, then later on the main loop links and body destroyed, `pw_core_sync`,
and on its done `lost(detail)`. Loss details: `the <kind>'s link for channel <n> failed: <detail>`
and `the <kind>'s link for channel <n> is gone`; a body's own `failed` detail as it gives it.

The two bodies:

| | Card (`sound_cards.cpp`) | RTP receiver (`rtp_receivers.cpp`) |
|---|---|---|
| Made by | `pw_core_create_object("adapter")` on ear's connection, as today | `pw_context_load_module("libpipewire-module-rtp-source", <section 5's string>)` |
| Found by | its own proxy | a registry binding on ear's core and a `pw_core_sync`: node globals named `engineNode` seen before its done are stale; after it the module is loaded, and the first node global named `engineNode` that is not stale (a stale id is forgotten on its `global_remove`) is bound for its info |
| Ports | `PortConfig` set by `pw_node_set_param`, as today: `playback_AUX<i>`, `capture_AUX<i>` | `adapter.auto-port-config` in `stream.props`: `receive_AUX<i>` |
| `running(id)` | info RUNNING; id = `pw_proxy_get_bound_id` (SPA_ID_INVALID: `failed`) | info RUNNING of the bound node; id = its global id |
| `failed` | info ERROR, proxy error, proxy removed (today's details) | load returns null: `cannot load PipeWire's rtp-source module: <strerror>`; info ERROR: `the RTP receiver failed: <error>`; its global removed: `the engine removed the RTP receiver`; module destroyed: `the engine removed the RTP receiver's module` |
| Destructor | hooks removed, proxy destroyed | hooks removed (module, node, registry, core), node proxy and registry destroyed, `pw_impl_module_destroy` if the module is still there |

`sound_cards.hpp`'s comment changes only where it says the links name the card by node name.

### 7.3 ear_nodes

`builtin.hpp` gains `std::unique_ptr<const NodeType> makeRtpNodeType();` (section 3's grammar,
ports and pass-through). R1c registers it: `builtinNodeTypes` becomes white, file, card, rtp, and
its comment drops "from slice R1 of docs/design/rtp.md".

The `rtp` config's `start(grant)`: `grant.rtpReceiver({address, port, encoding, rate or
grant.format().rate, latencyMs, ports().insideInputs})`, then insides whose audio code copies
input `i` (the inside input `from_rtp_i`; the node has no public inputs) to output `i`.

### 7.4 Test support

`test/support/rtp_tone.hpp` (new, declarations; R1b writes the bodies; INTERFACE target
`ear_rtp_tone` in `test/CMakeLists.txt`): `RtpPcm { l16, l24 }`, `RtpTone { frequencies,
amplitude }`, `RtpSender(port, pcm, rate, tone, ssrc = 0xC0FFEE)` with `send(frames)` and
`waitSent(deadline)`, `freeUdpPort()`, and `fitSine(samples, frequency, rate) -> SineFit {
amplitude, residual }`.
The sender paces packets on the steady clock: it stands in for the sending device's own clock.
It never decides when a test step happens; tests wait for events (a line ear prints, a
`waitSent`, a level message, a main-loop event).

`src/pipewire/tests/engine_harness.hpp` (new in R1b): `makeEngine`, `Waiter`, `NoBell`,
`BlockEvents` and `Registry`, moved out of `sound_card_test.cpp` unchanged, so the card's and the
receiver's engine tests share one copy.

### 7.5 Wiring

`main.cpp` makes `ear::pipewire::RtpReceivers receivers(*engine)` beside the cards and passes it
to `NodeHost` (done in the interface stage).

## 8. Slices of the tracer milestone

Order: **R1a** (ear_core, the package the others import) → **R1b** (ear_pipewire) and **R1n**
(ear_nodes) in parallel (disjoint files and outputs; both need R1a's `RtpAddress::parse`) →
**R1c** (join: registration and the system test). Each is green alone: until R1c no node type asks
for a receiver. Land command per slice: `git add -A -- <owned> && git commit -m '<msg>' --
<owned>`, then push to `claude/fpp-ear-audio-source-640981`. A slice that needs a file outside
its list, a dependency or a signature change appends to section 13 and stops.

| Slice | Builds | Owned files | Size |
|---|---|---|---|
| R1a core | `RtpAddress`; `NodeHost` keeps one list of outside requests (cards and the receiver), one naming function, one ready and loss path; the grant's `rtpReceiver` | `src/core/rtp_receiver.cpp` (new), `src/core/node_host.cpp`, `src/core/CMakeLists.txt`, `src/core/tests/rtp_receiver_test.cpp` (new), `src/core/tests/node_host_test.cpp` | 3-4 h |
| R1b engine | `joinOutsideNode` (the card's lifecycle moved out of `AlsaSoundCard`), the card's body, the receiver's body, the harness and sender | `src/pipewire/outside_nodes.cpp` (new), `src/pipewire/sound_cards.cpp`, `src/pipewire/include/ear/pipewire/sound_cards.hpp` (comment only), `src/pipewire/rtp_receivers.cpp`, `src/pipewire/CMakeLists.txt` (+ `outside_nodes.cpp`, tests link `ear_rtp_tone`, `rtp_receiver_test.cpp`), `src/pipewire/tests/engine_harness.hpp` (new), `src/pipewire/tests/sound_card_test.cpp` (helpers moved out, nothing else), `src/pipewire/tests/rtp_receiver_test.cpp` (new), `test/support/rtp_tone.hpp` | 6-7 h |
| R1n type | the `rtp` type: section 3's parse, ports, pass-through, start | `src/nodes/rtp.cpp` (new), `src/nodes/CMakeLists.txt`, `src/nodes/tests/rtp_test.cpp` (new) | 2-3 h |
| R1c join | `builtinNodeTypes` with rtp; the owner's end-to-end tests through `out/ear` | `src/nodes/builtin.cpp`, `src/nodes/include/ear/nodes/builtin.hpp` (the `builtinNodeTypes` comment only), `src/nodes/tests/white_test.cpp` (the type list), `test/system/rtp_test.cpp` (new), `test/system/CMakeLists.txt` | 2-3 h |

Built after the owner stop, not in this milestone: section 9.

### Acceptance criteria and mutation probes

Every test waits for events; a deadline only fails a test. One probe per criterion; the verifier
applies it, sees the test go red, and restores it.

**R1a (ear_core, `[rtp_address]`, `[node_host]`).**
1. `RtpAddress::parse` accepts `0.0.0.0`, `224.0.0.1`, `239.69.0.1`, `239.255.255.255` (text as
   given; `multicast()` false only for `0.0.0.0`). Probe: `multicast()` true for `0.0.0.0`.
2. It refuses `""`, `127.0.0.1`, `192.168.1.10`, `10.0.0.1`, `223.255.255.255`, `240.0.0.1`,
   `255.255.255.255`, `fpp.local`, `::`, `ff02::1`, `239.1.1`, `239.1.1.1.1`, `239.01.0.1`,
   `239.256.0.1`, `" 239.1.1.1"`, `"239.1.1.1 "`, `0x7f.0.0.1`, `239.1.1.1/24`. Probe: accept a
   unicast address (drop the multicast range check).
3. A start asking for a receiver: nothing is made before the block is ready; then one
   `RtpReceiverFactory::make` named `<node>.rtp`, blockNode = the node, the request unchanged
   (address, port, encoding, rate, latencyMs, blockPorts). Probe: name it `<node>.receiver`.
4. With a card and a receiver, the node is `running` only once both are ready, in either order.
   Probe: insides ready on the first outside node's ready.
5. A receiver lost after the node ran: `failed engine-refused: <its detail>`; routes and block
   stay; `node.update` restarts it (the old receiver destroyed, a new one made once). Probe: a
   loss after ready is ignored.
6. `rtpReceiver` outside start, and a second `rtpReceiver` in one start, throw
   `std::logic_error`. Probe: drop the second-request check.
7. Restart and removal destroy the receiver before the handoffs close and before the insides go
   (the fakes log the order). Probe: destroy outside nodes after the insides.
8. Every existing `[node_host]` card test passes unchanged. Probe: none (a regression check).

**R1b (ear_pipewire, `[rtp]`, real engine; the sender on 127.0.0.1 at `freeUdpPort()`).**
1. A receiver (L24, 2 channels, rate = graph rate) for a block with inside inputs `from_rtp_1`,
   `from_rtp_2` is told `ready` once (no `lost`); the receiver's node and every block report the
   Dummy-Driver as `node.driver-id` (bound afresh). Probe: add `node.driver = true` and
   `priority.driver = 300000` to `stream.props`, so it drives [U: that it then drives].
2. Sent 750 Hz on channel 1 and 1500 Hz on channel 2 (amplitude 0.5), the block's inside input
   1 fits 750 Hz with amplitude 0.5 ± 0.01 and residual below 0.1, and its 1500 Hz amplitude is
   below 0.01; input 2 the other way round, over 1 s of the received span. Probe: join channels
   in reverse order; second probe: pass `S16BE` for L24.
3. L16 sent at 44100 into the 48000 graph fits 750 and 1500 Hz (resampled, not 816 and
   1633 Hz). Probe: leave `audio.rate` out of the arguments (the module's default is 48000).
4. A sender that restarts with a new SSRC (a second `RtpSender` with another `ssrc`, started
   once the first has `waitSent`) is heard: the block hears the tone again after the second starts. Probe: drop
   `sess.ignore-ssrc`.
5. A receiver whose node goes (its node global destroyed through the registry) is told `lost`
   after make returned, its node and links absent from the registry when told, the blocks still
   on the Dummy-Driver and processing. Probe: tell `lost` before the teardown's sync.
6. Make, destroy and at once make again for the same names (a restart): the second is ready and
   never lost, and the links name its node's id. Probe: drop the stale-id check [U: whether the
   race shows on this engine; the verifier reports if the probe stays green].
7. Every `[cards]` test passes unchanged after the cards move onto `joinOutsideNode` and the
   harness move. Probe: in `joinOutsideNode`, report ready on the first active link (the card's
   readiness test goes red).

**R1n (ear_nodes, `[rtp]`).**
1. A full, valid settings object parses; ports are outputs `channel_1`..`channel_N` and inside
   inputs `from_rtp_1`..`from_rtp_N`, nothing else. Probe: name the outputs `out_<i>`.
2. Start asks the grant for exactly one receiver: address, port, encoding as set, blockPorts =
   the inside inputs; `rate` absent takes `grant.format().rate` (a grant at 44100 gives 44100);
   `latencyMs` absent gives 100. Probe: default the rate to 48000.
3. Each row of section 3's table is refused with exactly its detail (missing, wrong type, out of
   range at both ends, unknown key, not an object). Probe: accept `channels` 65.
4. The audio code copies `from_rtp_i` to `channel_i` exactly, for 1 and 3 channels. Probe: copy
   input 0 to every output.
5. `L24` gives `RtpEncoding::l24` and `L16` gives `l16`. Probe: map both to `l16`.

**R1c (`[system][rtp]`, the real `out/ear`).**
1. A graph file with `rtp` node `fpp` (`address` `0.0.0.0`, a free port, `format` `L24`,
   `channels` 2, no `rate`) and `file` node `rec`, routes `fpp:channel_1 -> rec:FL` and
   `fpp:channel_2 -> rec:FR`. Once ear prints `node fpp running` and both routes `linked`, the
   test sends 1.5 s of 750 Hz and 1500 Hz at 48000 (amplitude 0.5); once `waitSent`, SIGTERM.
   ear exits 0 and prints `node rec stopped dropped=0`; the WAV's FL fits 750 Hz and FR 1500 Hz
   (amplitude 0.5 ± 0.01, residual below 0.1, the other tone below 0.01) over 1 s inside the
   received span, which is at least 1.2 s long. Probe: in the `rtp` type, copy input 0 to every
   output (FR then fits 750 Hz).
2. With no sender, `node fpp running` is printed and a `levels` message shows `fpp`'s
   `channel_1` with peak 0 (Q1: running and silent). Probe: tell the receiver ready only after
   the first packet (it never runs).
3. `builtinNodeTypes` is card, file, rtp, white (`white_test.cpp`). Probe: leave rtp out.

## 9. After the owner stop (not built in this milestone)

- **A port that cannot be bound** (finding g) leaves the node `starting` with silent outputs and
  the reason only in PipeWire's log. Ways to make it a failure, for the owner to pick: (1) ear
  binds the same port itself just before loading the module (same options, `SO_REUSEADDR`) and
  refuses with the error: real reasons (in use, permission, no multicast route) at the cost of a
  check that races with other processes and copies the module's socket rules; (2) a later
  PipeWire that reports it; (3) accept `starting`.
- Multicast receive tested (loopback multicast in the container), and the Kubernetes side
  (`hostNetwork` or a multicast-capable network; a UDP Service carries unicast only).
- `net.mtu` raised so many channels fit a datagram (finding 3); a `latencyMs` bound tied to the
  ring (finding 5).
- A "receiving" status from `rtp.receiving` (finding h): deferred by Q1.
- Two `rtp` nodes on one port: both bind (`SO_REUSEADDR`) and unicast packets reach only one.
  A desired-graph check could refuse the second.
- IPv6 (`::` and `ff00::/8`), and a sender-address filter (finding e2: it also needs the sender's
  source port).
- Docs: `AGENTS.md` (the type, its settings and ports), the `pipewire-engine` skill (rtp-source
  in-process: `remote.name`, auto port config on a stream, `always-process` for RUNNING,
  `ignore-ssrc`, the silent bind failure, the blocking name lookup, the MTU), the `ear-on-talos`
  skill (FPP's stream into the pod), this page's results.

## 10. Test architecture

| Module | Isolation | Through |
|---|---|---|
| `RtpAddress` | pure | `parse` only |
| `NodeHost` with receivers | fake `RtpReceiverFactory` and `SoundCardFactory` whose events the test tells | `DesiredNodeListener` calls, `NodeStatusListener` lines |
| `rtp` type | fake `NodeGrant` that records requests; spans in, spans out | `NodeType::parse`, `NodeConfig::start`, `AudioCode::process` |
| `joinOutsideNode` | through its two kinds on the real engine: the card tests (test ALSA card) and the receiver tests (sender thread) | `OutsideNodeEvents`, the registry |
| `RtpReceivers` | real in-process engine, no daemon, a `pw_filter` block, a sender on 127.0.0.1 | `RtpReceiverFactory::make`, the block's code |
| whole path | the real `out/ear` on a graph file, the sender, the WAV | stdout lines, the control port's levels, the WAV |

Conformance boundary: `OutsideNodeEvents` (ready at most once, lost at most once and never inside
`make`, everything torn down before `lost`) is checked for both kinds by the same kinds of test
(R1b(1), (5) and the card's E2 tests). The engine tests leave out what CI cannot do: real FPP,
multicast, a real network's loss and jitter.

## 11. Costs and risks

- An open UDP port with no login: anyone who reaches it can play audio into the graph, as anyone
  who reaches port 7777 can edit it.
- About 100 ms of latency (R2), more than the system design's 10 ms ideal; this source only.
- A node whose port cannot be bound says `starting`, not `failed` (section 9).
- `running` while nothing arrives (Q1). The meters show the silence.
- The module runs its own internal connection per receiver (one more client in the engine).
- Moving the card onto `joinOutsideNode` changes how its links name it (by id instead of name);
  E2's tests must stay green unchanged (R1b(7)).
- The tracer tests unicast on 127.0.0.1 only; FPP's real stream (format, rate, channel count,
  multicast or not) is still unknown (R1).

## 12. New terms

| Term | Meaning | Why needed | Could be |
|---|---|---|---|
| Outside node | a PipeWire node a node's insides ask ear for, joined channel for channel to the node's inside ports, torn down with the insides; sound cards and RTP receivers are its two kinds | one lifecycle for both kinds | batch 2's "sound card" (one kind of it); not "engine node", which already means every PipeWire node |
| RTP receiver | an outside node made by PipeWire's rtp-source module for one stream named in settings | the `rtp` type | "RTP source" (PipeWire's name; ear's "source" means a node with outputs) |
| Body (of an outside node) | one kind's PipeWire object: how it is made and heard from | so the lifecycle is written once | strategy (pattern) |
| Stale node | a node of the receiver's name still in the registry from before (a restart's old receiver) | so the receiver binds its own node | — |

## 13. Errata

1. **(2026-10-31, tracer implementer) One sine fit over a whole second is the wrong check for a
   received tone: R1b criteria 2-4 and R1c criterion 1.** Alone, each tone test passed. Under
   `ctest --parallel 8` in the dev image (8 CPUs, no realtime scheduling) they failed in about 1
   run in 4 (and in 11 of 12 runs with a sender woken every 1 ms). The amplitude in each tenth of
   a second was 0.4999 throughout, with no gap, yet the 1 s fit read 0.489 with residual 0.15. The
   receiver steers its resampler with a delay-locked loop driven by the packets' arrival
   (section 4, finding d), so jitter in arrival moves its rate by parts in ten thousand, and over
   1 s that moves a 1500 Hz sine's phase by about a radian. Proposed: check every tenth of the
   same 1 s window (amplitude 0.5 ± 0.01, residual below 0.1, the other tone below 0.01). An
   unresampled 816 Hz for 750 Hz, a swapped channel or silence still fails every tenth, so each
   probe still bites. The windowed check belongs in `test/support/rtp_tone.hpp` beside
   `fitSine`, so both test files share it; that header is frozen (section 7.4), so the
   implementation repeats the loop in each test file until a re-cut adds it.
2. **(2026-10-31, tracer implementer) The tests' sender pace touches the other engine tests.**
   With the sender polling every 250 µs, the existing ramp tests ("A link carries a source
   block's ramp…", "A block with inside ports…") and the control-port system test's noise RMS
   failed under the same parallel ctest in 4 of 14 runs. Without the RTP tests the result was 0 of
   12. CPU use is not the cause (0.2 s of CPU in a 2 s test). The candidate is the wake rate in
   Docker's VM. The sender now wakes once per packet on an absolute schedule (`sleep_until`), but
   the dev image's Docker stopped mounting `/Volumes` before that build ran, so this is NOT
   VERIFIED.
