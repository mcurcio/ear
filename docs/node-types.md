# Node types

ear has five built-in node types. Each is its own library in `src/nodes/<type>/`, declared by one
settings table and one list of port banks ([writing-a-node-type.md](writing-a-node-type.md)).
Settings are written in a node's braces in `ear.conf` ([graph-file.md](graph-file.md)) or sent as
JSON on the control port; the refusal texts below are exactly what `invalid-settings` carries.
How every node starts, stops, fails and restarts is the framework's, the same for all types
([architecture.md](architecture.md#the-node-lifecycle)).

## Ports at a glance

| Type | Inputs | Outputs | Inside banks (no route can name them) |
|---|---|---|---|
| `white` | none | `noise.mono` | none |
| `file` | `record.left`, `record.right` | none | `writer` (its sink) |
| `card` | `play.1`..`play.N` (audio going to the card) | `capture.1`..`capture.M` | `to_card`, `from_card` |
| `rtp` | none | `stream.1`..`stream.N` | `from_rtp` |
| `sendspin` | `to.left`, `to.right` | `from.left`, `from.right` | none |

**How settings are checked.** The table checks a settings object in a fixed order, so the first
problem a refusal names is stable: not an object; then an undeclared key (the first in sorted
order); then each row in the table's order (missing, then a key given with one it may not be
given with, then the kind and the range or meaning); then the type's own rule across settings. A
whole number must be a JSON integer (`5004.5`, `"5004"` and `true` are not), and so must a rate,
in hertz (in `ear.conf` a rate is written with its unit:
[graph-file.md](graph-file.md#rates)). Every refusal's text
and order is tested in `test/nodes/settings_refusals_test.cpp`.

## `white`

Uniform white noise in [-1, 1) on one output, `noise.mono`. It takes no settings: any key is
`unknown key <key> (white takes no settings)`. It acquires nothing, so it runs as soon as its
block is ready.

```conf
node white noise;
```

## `file`

Records its two inputs into a 32-bit float stereo WAV file at the graph frequency.

```conf
node file rec { path "take1.wav"; }
```

| Key | Kind | Rule | Refused with |
|---|---|---|---|
| `path` | text, required | Relative to the recordings folder (`--recordings-dir`): non-empty, not absolute, no `..` component | `path: missing`, `path: must be a non-empty string`, `path: must be relative to the recordings folder, without ".."` |

- **Where it writes.** The file is opened beneath the recordings folder only (`openat2` with
  `RESOLVE_BENEATH` on the folder's descriptor, held from boot), so a symlink that leads out fails
  the node's configure, and so does a missing subfolder (none is made). The file is created or
  truncated at configure.
- **How it writes.** It writes no audio code: the framework's pass-through copies `record` to its
  inside bank `writer`, whose sink holds 2 s of ring. The WAV is opened on the sink's own thread,
  never the main loop or the realtime thread; if libsndfile refuses it, the node fails
  `start-failed: cannot open <path>: <reason>` before it is configured, and if the open has not
  finished within 5 s, `start-failed: the sink writer's consumer did not begin within 5 s`.
- **What it records.** Every cycle its code drove, sample for sample (its bank is never faded), and
  none while it is stopped: a stop closes nothing, a start appends again. The file is complete once
  its configuration is torn down (a restart, a disable, a removal, or ear stopping). A restart
  finishes the old file and truncates and opens the path afresh.
- **Drops.** A full ring drops frames and counts them; the count is the `dropped=` of the node's
  `removed` record and `droppedFrames` on `node.gone`. A short write (a full disk) is said once, as
  a note, `write-short <path>: <reason>`.

## `card`

A sound card through ALSA, behind the node's port block.

```conf
node card desk { card MOTU; playback 2; }
node card mic  { card USB2; capture 1; }
```

| Key | Kind | Rule | Default | Refused with |
|---|---|---|---|---|
| `card` | text, required | The ALSA card id as in `/proc/asound/cards`: 1 to 15 of letters, digits, `-`, `_`. Never an index or an ALSA device string | | `card: must be an ALSA card id, 1-15 characters of [A-Za-z0-9_-] (as in /proc/asound/cards)` |
| `playback` | whole number | 0 to 64 channels | 0 | `playback: must be a whole number of channels, 0-64` |
| `capture` | whole number | 0 to 64 channels | 0 | `capture: must be a whole number of channels, 0-64` |
| (across) | | at least one of `playback` and `capture` above 0 | | `playback or capture: at least one must be above 0` |

- Inputs `play.1`..`play.N` (N = `playback`) pass straight through to the inside bank `to_card`,
  joined to a playback ALSA node `<node>.playback`; a capture ALSA node `<node>.capture` feeds the
  inside bank `from_card`, which passes to outputs `capture.1`..`capture.M`. The type writes no
  audio code.
- The card opens at the graph frequency, as `hw:CARD=<id>,DEV=0`. *Why only a card id:* no user
  text reaches ALSA unparsed, and ALSA's `file` plugin can run commands.
- **The card never drives the graph's clock.** ear's internal clock always does; a linked card
  follows it, resampled to it, so no output is bit-perfect, and its rate takes a few seconds to
  settle after it starts ([engine.md](engine.md#sound-cards)).
- The node is `running` once the card's PCM runs and every channel is linked. A card that cannot
  open, or is lost later (unplugged, a rate the kernel refuses, a link that fails), makes the node
  `failed engine-refused: <detail>`; its block and routes stay, and the rest of the graph keeps its
  clock. A card that opens but whose PCM has not run within 10 s fails it `engine-refused: the card
  never started: <card id>` ([engine.md](engine.md#readiness-deadlines)). There is no automatic retry: `node.restart` or `node.start` opens the card afresh, as
  does `node.update` with changed settings; an update with the same settings changes nothing.
- A **stopped** card keeps its device open and plays the block's silence; a **disabled** one
  releases the device, so another program may use it.
- Changing `playback` or `capture` changes the node's ports, so the node is replaced (its routes
  are removed and added again).
- **A known limit:** ALSA's open, drop and close run on ear's main loop, so a USB card that wedges
  inside the kernel can block every node and the control port's graph requests until ear restarts.
  The main-loop watchdog reports it.
- **A MOTU (or any card that validates rates)** fails at open when the graph frequency is not the
  rate set on the card; that fails the card node only. The owner's MOTU 24Ao and its Linux quirks
  are the `ear-on-talos` skill's (`.claude/skills/ear-on-talos/SKILL.md`).

## `rtp`

Receives one RTP audio stream from the network, such as FPP's.

```conf
node rtp fpp { address "0.0.0.0"; port 5004; format "L24"; channels 2; rate 48khz; latencyMs 100; }
```

| Key | Kind | Rule | Default | Refused with |
|---|---|---|---|---|
| `address` | text, required | `0.0.0.0` (any sender) or an IPv4 multicast group, 224.0.0.0 to 239.255.255.255, as a plain dotted quad | | `address: must be "0.0.0.0" or an IPv4 multicast group (224.0.0.0-239.255.255.255), as a dotted quad` |
| `port` | whole number, required | 1 to 65535 | | `port: must be a whole number, 1-65535` |
| `format` | text, required | `L16` or `L24` (big-endian PCM) | | `format: must be "L16" or "L24"` |
| `channels` | whole number, required | 1 to 64 | | `channels: must be a whole number, 1-64` |
| `rate` | rate | 8 kHz to 192 kHz, the sender's rate: `44.1khz` in `ear.conf`, `44100` (Hz) on the control port | the graph frequency | `rate: must be a rate from 8khz to 192khz (on the control port, a whole number of Hz)` |
| `latencyMs` | whole number | 1 to 1000, the receive buffer | 100 | `latencyMs: must be a whole number of milliseconds, 1-1000` |

- Outputs `stream.1`..`stream.N`; no inputs. The receiver, `<node>.rtp`, feeds the inside bank
  `from_rtp`, passed straight through. The receiver's `latencyMs` is latency beyond `from_rtp`
  ([engine.md](engine.md#latency-per-bank-pair)), carried to `stream`, so downstream nodes are
  told it.
- The stream follows the graph's clock and never drives it. A sender at another rate is
  resampled to the graph frequency.
- **Why only those addresses:** a unicast address does not mean "this host's address" to
  PipeWire: it would hear only that sender, and only when it sends from the same port it sends to.
  A host name is refused because looking it up blocks the main loop (15 s for `fpp.local` once).
- **No packets is not a failure.** With no sender the node is `running` and silent (its meters show
  it). A sender that restarts (a new SSRC) is heard again.
- **A port another program holds fails the node after 5 s.** PipeWire only logs the failed bind, so
  the receiver never runs: the node stays `configuring` (state `starting`) with silent outputs, says
  after 2 s `node fpp still configuring after 2s: waiting for fpp.rtp` (`graph.get` shows
  `waitingFor` `companion` `fpp.rtp`), and once the receiver's 5 s readiness deadline passed it
  fails `node fpp failed engine-refused: the RTP receiver never ran (UDP port 5004 held?)`
  ([engine.md](engine.md#readiness-deadlines)). Free the port, then `node.start` or `node.restart`
  retries it. Two `rtp` nodes on one port both bind, and unicast packets reach only one.
- **A packet lost or out of order** resets the receiver, so about `latencyMs` of silence follows.
  Even one sending thread over loopback reorders packets under load.
- **Channel limits:** a datagram longer than 1280 bytes is cut short and garbled, so 1 ms packets
  at 48 kHz fit up to 8 channels of L24 or 13 of L16. The receiver's ring holds 4 MiB (about 455 ms
  of 64 channels of L24 at 48 kHz); a `latencyMs` near that overruns. `latencyMs` is rounded down
  to whole packet times (100 became 96.9 ms for 2-channel L16).
- Tested only with a sender on 127.0.0.1; multicast is not tested yet. Getting FPP's stream into a
  pod is the `ear-on-talos` skill's. The PipeWire facts behind all this are in
  [engine.md](engine.md#rtp-receivers).

**On an untrusted network.** The UDP port has no login, as port 7777 has none: anyone who reaches
it can play audio into the graph. Keep it reachable only from the sender's network.

- The receiver handles packets on the graph's one realtime data loop, and every resync (any
  sequence mismatch, or an overrun) zeroes its whole 4 MiB ring there. A flood of datagrams with
  changing sequence numbers (the source address and SSRC do not matter and can be spoofed) stalls
  every node's processing: cards underrun and `file` nodes drop frames. A `latencyMs` above what
  the ring holds for the stream does the same with no attacker.
- With `sess.ignore-ssrc` and `0.0.0.0`, a second sender is not dropped: two overlapping senders
  (an old FPP still sending while a new one starts) garble the audio for as long as both send.
- A malformed datagram logs one PipeWire warning on the realtime thread, so a flood of them floods
  the log; a group in `224.0.0.0/24` (which the grammar allows) brings steady non-RTP traffic.
- Each receiver costs a fixed 4 MiB ring, a connection and a socket, and the graph has no limit on
  its nodes: anyone who reaches port 7777 can add receivers until the pod runs out of memory.

**Not built:** a failure for a port that cannot be bound (ear binding the port itself first, or a
PipeWire that reports it); a "receiving" status from the module's `rtp.receiving`; multicast
tested, and a multicast-capable pod network; a larger datagram (`net.mtu`) for many channels, and
a `latencyMs` bound tied to the ring (`latencyMs * rate / 1000 * channels * bytesPerSample` under
4 MiB); packet handling on a data loop of its own (`context.data-loops` and the module's
`node.loop.class`); refusing `224.0.0.0/24` and other well-known groups; refusing a second `rtp`
node on one port; a limit on engine companions; SAP discovery; IPv6 and a sender-address filter; an
RTP sender.

## `sendspin`

Music Assistant's audio in and out, over the Sendspin protocol. Its outputs play what Music
Assistant sends; what is routed to its inputs is sent to Music Assistant.

```conf
node sendspin kitchen { server "ws://192.168.1.10:8927/sendspin"; name "Kitchen"; }   # dials
node sendspin porch { port 8928; name "Porch"; }                                      # listens, advertises
node sendspin study;                                                                  # listens on 8928, named "study"
```

| Key | Kind | Rule | Default | Refused with |
|---|---|---|---|---|
| `server` | text | `ws://<host>[:<port>]/<path>`: host a dotted quad or letters, digits, `-` and `.` (at most 253); path of `/` and `[A-Za-z0-9._~-]`; no `wss://`, user, query or fragment | none: listen | `server: must be ws://<host>[:<port>]/<path>` |
| `port` | whole number | 1 to 65535, only without `server` | 8928 | `port: must be a whole number, 1-65535`; `port: only without "server"` |
| `name` | text | 1 to 64 bytes of UTF-8, no control characters | the node's name | `name: must be 1 to 64 bytes of UTF-8 with no control characters` |

- Outputs `from.left`, `from.right`; inputs `to.left`, `to.right`. The server sends FLAC 24 stereo,
  resampled by it to the graph frequency; the node sends PCM 24 stereo at the graph frequency when
  the server asks. The node follows the graph's clock and never drives it.
- **With `server` it dials; without, it listens** on `port` and advertises `_sendspin._tcp` over
  mDNS. Never both: the spec forbids a client dialling while it advertises, and a dialling node
  opens no listening socket. A host name is looked up on the library's transport thread, never on
  the main loop.
- **The connection is not the node's health.** The node is `running` with no server, its outputs
  silent; the graph keeps running whatever the server does. The connection is told in notes
  (below).

### Pairing

Pairing by a code is required, for good: a server that approves the node without pairing gets no
roles and no audio (`says connected "<server>" unpaired`). To pair: get the node connected (Music
Assistant offers pairing only to a connected client, whichever side dialled), start pairing in
Music Assistant, read the code from `says pairing-code 123-456` on ear's standard output, and type
it there. `says paired`, then `says connected "<server>" paired` follows. A wrong code keeps the
same code showing.

The code is on standard output only: notes never reach the control port or the editor, so whoever
pairs needs ear's log open. 20 wrong rounds hold pairing back (`says pairing-held`) for this
client's life; the count is in memory, so `node.restart` (or a change of settings, or restarting
ear) releases it, while `node.stop` and `node.start` do not. Any network peer can burn these
rounds (the spec does not partition them by server or address).

### The identity file

Each node's identity and pairings are `<config-dir>/sendspin-<node>.key`, mode 0600: one JSON
object with audit fields (`createdAt`, `createdBy`, `changedAt`, `changedBy`) and the library's
blobs in base64url (private key, pairing secrets, output delay). At most 64 KiB, written whole on
each change through the confined folder, swapped in as `ear.conf` is. It holds a private key:
never log it or send it to the editor.

- A wider mode is narrowed, not refused, at each boot for every key file in the folder (whatever
  its node) and at each start and save. *Why:* a Kubernetes `fsGroup` widens it on every mount.
  What boot cannot narrow is a `log` record from source `sendspin`.
- Renaming a node makes a new identity (re-pair); `node.remove` leaves the file, so a name added
  back inherits it. To wipe an identity, delete the file and `node.restart` the node.
- A file it cannot trust (not JSON, a missing field, a key of the wrong size, a symlink, too
  large) fails the node, left unchanged but for its mode: fix or delete it. No new identity is
  minted over it, or the server's trust would be lost silently. A missing file in a folder it
  cannot write fails the node too.
- A key file something else changed or removed since it was read fails the write
  (`key-write-failed`), and theirs stands.

### Notes

A note is `node <n> says <text>` on standard output (a `note` record), never a state. Notes pass
32 at once, then one a second; `notes-dropped <n>` says how many were held back, before the next
that passes.

| Note | Means |
|---|---|
| `id <client id>` | First, once: the node's client id |
| `dialling <url>` | Dialling, until connected. With `running` and silent meters: no server yet, not a failure |
| `listening <port> as ear-<node, first 50 bytes>-<client id, first 8>` | Listening; the mDNS instance name |
| `connected "<server name>" paired\|unpaired` | Connected (the name made printable: control characters, bad UTF-8 and quotes become `?`, at most 64 bytes) |
| `disconnected` | The connection read lost for 5 s (it reads lost through a re-handshake, hence the 5 s). A dialling node redials then, and every 31 s until connected, so a server back after the first redial can take up to 31 s to be heard |
| `pairing-code <123-456>` | The code to type into Music Assistant, shown until cleared |
| `pairing-code-cleared` | The code is no longer valid |
| `paired <server id>` | Paired |
| `pairing-failed <reason>` | `attempt-timeout`, `concurrent-attempt`, `method-not-supported`, `pairing-code-mismatch`, `user-cancelled` or `unknown` |
| `pairing-held` | 20 wrong rounds: attempts held back |
| `stream <flac\|pcm> <rate> <channels> <bits>` | A stream from the server began. A format ear did not list leaves the sink full and silent, and the library closes the connection when its ring pins |
| `stream-ended` | The stream ended |
| `sending <rate> 2 24` | Sending to the server |
| `sending-stopped` | Sending stopped |
| `key-write-failed <detail>` | The pairing holds for this run only: fix the folder and pair again |
| `advertise-failed <detail>` | Listening but not advertising: give the server `ws://<address>:<port>/sendspin` |
| `notes-dropped <n>` | Notes held back by the rate |

### Failures

| What | The node | What to do |
|---|---|---|
| No server, or the server gone | `running`, outputs silent, inputs dropped; `says disconnected` | Nothing: it redials |
| The listening port is held by another program or another `sendspin` node | `failed start-failed: cannot listen on port <p>: held by node <n>` or `: <reason>` | `node.update` to a free port. A program taking the port just after the check is not caught: the library only logs, and retries every 5 s |
| A key file it cannot trust, or the insides dying | `failed start-failed: <detail>` (or `lost` once it ran) | Fix the cause, then `node.restart` or `node.start`; a change of settings or a restart of ear also retries |
| The mDNS socket cannot open | Listening, not advertising; `says advertise-failed` | Give the server the URL |
| A later key write fails | `says key-write-failed` | Fix the folder; pair again |

### Stop, restart, disable

A stopped `sendspin` node keeps its connection, pairing and library running, silent: nothing it is
sent is played or reported played, and nothing is sent. Up to about 43 ms the feed held from before
plays first at the next start. A restart, a disable or a removal says goodbye (`shutdown`) and
drops the connection; a restart or an enable builds a new client from the key file. A disabled node
holds no port and no connection. A name added back waits for its old node to go.

After a `node.update` or `node.restart` the goodbye is `shutdown`, and a server that dialled by hand
(`aiosendspin`'s `connect_to_client`) did not redial within 30 s in tests, though the spec has a
server reconnect on that reason; one browsing mDNS comes back by itself.

### mDNS

- One socket per node on port 5353 (shared with `SO_REUSEPORT`), one interface: the one the kernel
  routes 224.0.0.251 through, its address read once at the node's start. A pod on an overlay network
  cannot advertise.
- Answers go by multicast whatever a question's unicast bit; only a querier on another port than
  5353 gets a unicast answer. *Why:* the kernel hands a unicast datagram to one socket of a shared
  port, so it could reach a neighbour's.
- Limits, so it reflects nothing: queries only (responses ignored), from this link only, each answer
  once per datagram, by multicast at most once a second, by unicast at most 16 a second. A querier
  that asks again within the second is answered the next time it asks.
- The instance name is `ear-<first 50 bytes of node>-<first 8 of client id>`, a DNS label's 63 bytes
  at most, stable and unique without probing. Music Assistant shows `name`, from the hello.
- A goodbye goes out when the worker stops; a crash leaves the record until its TTL.

### On an untrusted network

A listening node has no address setting: port 8928 and mDNS's 5353 are on every interface. Dial
instead, or filter both, if that matters. What a peer can and cannot do:

- An upgrade carrying an `Origin` header is closed before any Sendspin message (the spec says a
  client MUST reject it; a carried patch does it). The peer sees 101, then the close.
- A frame or message over 1 MiB, or over 1024 fragments, is refused before it costs anything.
- **Sockets that prove nothing give way to newcomers**, so holding them keeps no server out: an
  address holds at most 8 sockets still to upgrade and the node at most 32, both counting only
  sockets whose peer has not closed (a burst of connects closed at once refuses no one after it,
  unless it outruns the 64-thread bound, which every address shares), and the transport at most
  64 threads, closed or not; in the upgraded room, the newcomer's own
  address's oldest goes first, then one with its Noise handshake unfinished, then the oldest. A
  connection that proved a long-term pairing never gives way.
- **A sustained flood keeps servers out for as long as it lasts** (measured on loopback: about 16k
  connects a second from 4 addresses, or 9.6k from the server's own; slower floods cost a server a
  few seconds), and only stopping it lets them in. The owner accepted this cost.
- **A peer that finishes the published Sentinel handshake and activates nothing** holds the node
  from an unpaired server, and from a paired one that has never played (the spec's rule). The owner
  accepted this cost too.
- A peer behind the server's own address (NAT) can knock it off while it is still in its handshake,
  which costs it a reconnect.
- The protocol library and `mdns.h` parse what any peer sends, unfuzzed, inside the audio process:
  a crash silences every node.

### Clock

- Audio time is the graph's. The node tells the library when frames play from the cycle's start on
  the raw clock (`GraphCycle::startRawNs`) plus the latency downstream of its outputs (a following
  card's buffer, carried back by the framework), never from when the thread woke. *Why:* wake time
  failed the spike, 52% of cycles within 1 ms against 100% from the cycle's start.
- The finish it tells is where the library's next frame plays, the end of the telling cycle: the
  library puts its next audio at the finish plus what it still holds, and a cycle the feed left
  short played zeros to its end.
- The audio sent to the server (`to`) is dated by when its cycle started, each piece from the
  newest cycle's start by its frames ([engine.md](engine.md#dating)). The capture latency upstream
  of `to` is not counted yet, so a card's capture reaches the server dated late by the card's
  buffer; it is counted once `to` is a sink's bank, whose chunks carry it (slice 3c).
- **The node's clock is an honest sink with a local skip** ([the timing model](architecture.md#the-timing-model),
  rule 3): it plays every frame the library wrote, in order, and tells the library what played and
  when, true when told. It keeps no debt and no model of the library: what it could not skip of lost
  time, the library sees in the next finish it is told and corrects once, on its own.
- **It skips only the graph's lost time.** On the cycle that states a catch-up or a pause
  (`GraphCycle::facts`: cycles missed, the position moved by exactly their frames, cut or not;
  [writing-a-node-type.md](writing-a-node-type.md#lost-cycles)) with a jump above 0, overdue or not,
  it skips min(jump, what the feed holds now): that audio is stale. Every other cause (a
  reposition, a driver change, a drive, a first cycle, the cycle after an oversized one) is a new
  clock and skips nothing, and nothing of a stream is skipped on the cycle that applies a clear
  (the clear ended the stream the jump was lost from). A cycle the feed leaves short plays zeros,
  which are not counted.
- **It counts every frame.** The frames told played grow by exactly what was skipped and read,
  never the zeros of a short cycle nor anything of a cleared stream: the library takes what it is
  told played from what it wrote (clamping at zero with a warning), so a frame it did not write,
  told played, would be taken from audio still to play.
- **It tells only from a cycle that began on time** (`GraphCycle::overdueNs` 0). *Why:* the driver
  catches a stall up in a burst of timeouts, of which the block runs as many as the host lets it,
  from none to every one ([engine.md](engine.md#the-internal-clock)). A burst's cycle starts on the
  driver's own time, behind now by the graph's lag until the burst ends, and it runs at now; neither
  is where the library's next frame plays while the burst runs. Told the nominal start, the library
  would see its playtime behind now and, without the floor patch, floor it at now mid-burst; told
  the run time, in a burst that runs every cycle and loses nothing it would find its next chunk late
  by the lag, drop it, run that far ahead once the burst ended and add it back as silence: two
  corrections where nothing was lost. Its frames are told with the first on-time cycle's report,
  dated by that cycle's start, which is then true. A cycle on time tells even if it lost frames.
- **The deferral has one bound**, `untoldBound`: when the library has gone untold for 2 s of the
  graph's nominal time (the cycles' `startRawNs`, so a burst of short cycles does not stretch it),
  it is told from the next cycle even if that one is overdue, so a host that never runs a cycle on
  time does not leave it untold for good. That report is dated by when the cycle really ran
  (`GraphCycle::ranRawNs()`, [engine.md](engine.md#graphcycle)), and the report after it is clamped
  to it: dated by its own nominal start, earlier than the forced report's run time by up to its
  overdue time, it would step back. The clamp has no other case: reports from on-time cycles follow
  one another by their nominal starts, and a unit row holds it idle otherwise. **A finish may step
  back when what lies downstream shrinks** (a following card's latency told smaller): that is
  intended, since the finish is true and the library re-anchors on every report. The library may correct after a forced report: the bound's
  price.
- **While ear defers, the library's playtime runs open loop** (the last finish plus what it wrote
  since), trailing now through a burst of either shape with frames still buffered; upstream's floor
  would take that for an underrun and realign mid-burst, so the floor patch, which floors only when
  nothing is buffered, is carried with the deferral ([below](#library-facts-and-carried-patches)).
- **After a stall past the feed** the library corrects once what the feed did not hold (the stall's
  lost frames less the skip) and the zeros ear played meanwhile (a skip that empties the feed leaves
  its cycle to read nothing), at the next chunk it syncs: until then, what it had already written
  (the feed and the rest of its chunk) plays late by that much, and ear's own silence after the
  burst is at most a block, while the library refills the feed. *Why not pay it locally:* a node
  that owed the rest kept a model of the library to know when it could, and the library corrected
  the same time again whenever the model was wrong. The trade, accepted by the owner: up to about a
  chunk played late where the old wrapper played ear's silence. A stall the feed holds (a missed
  cycle or two) is skipped exactly and the library corrects nothing. A burst that runs every cycle
  loses nothing, so it needs no correction and plays no silence. The clock tests prove each on the
  real server with stalls of the data thread of 200 ms and 1 s and a 1.5-cycle hiccup, made
  upstream of the block so it misses the stall and its burst on any host, and the same stalls made
  inside the block, judged in whichever shape the host gives and recorded
  (`out/Testing/sendspin-stall-shapes.txt`, which CI prints): with frames lost, one correction of what was not
  skipped and the zeros, within the library's 500 us settle threshold, and at most one settle step
  of up to two blocks after it, only where a cycle between was dry or began with the feed empty
  (its next write landing late, as after a network gap); with none, no correction, no silence and nothing told from an overdue cycle. Every report's frames and finish are checked
  against the rule, and a finish behind the last fails.
- The feed (the library's decoded audio waiting for the block) is about 43 ms at 48 kHz, small on
  purpose: with 2.7 s a correction lands 2.7 s late. The node declares it as its latency on `from`.
- Only routed paths count toward the downstream latency (unrouted, or routed to a `file` node, it
  is 0). A resampler before the card and anything past the card are left to the output delay in
  Music Assistant, which the library keeps in the key file.
- Measured: within 1 ms on the dummy driver and through a following (clockless) test card. A
  correction is one frame per chunk at most (-0.084% over 150 ms against the spec's ±0.5%). On real
  hardware it is a check in the `ear-on-talos` skill, not yet run.

### Threads

The library never touches a realtime thread or the main loop (`notify_audio_played` and
`write_audio` take mutexes; `stop()` and the destructor can wait).

| Thread | Runs |
|---|---|
| Data loop | The node's code: reads the feed, writes the handoff to the server, publishes what played through a mailbox and rings a bell |
| Worker `ear-sendspin <node>` | The library's "main loop": the key file, start, a 10 ms `poll`, stop and destruction; every listener callback but `on_audio_write`; the connection watch; mDNS |
| Notifier `ear-notify <node>` | Waits on the bell, then `notify_audio_played` (apart from the worker, whose disk writes would delay it) |
| Handoff thread | `write_audio` |
| The library's own | Protocol, sync (`on_audio_write`), source, IXWebSocket's |

Teardown: the block goes silent, the handoff drains, then the worker says goodbye, stops and joins
the notifier, stops and destroys the client, and posts `closed`, never waiting on the main loop. A
dial in flight cancels within a poll; a half-open inbound socket takes up to 3 s.

### Library facts and carried patches

- sendspin-cpp is pinned at a master commit, not a tag: `accept_inbound_connections` came after
  v0.9.5. The pins and patches are in `cmake/SendspinDeps.cmake`; a patch that will not apply, or a
  tree without all of it, stops configure.
- `cmake/sendspin-listener.patch`: refuses an upgrade carrying `Origin`; makes an unproved
  connection give way in the nursery; lengthens the listen queue (its 5 dropped a server's SYN under
  a burst). The worker's `server_max_connections` is 32 (the library's default is 4).
  IXWebSocket's client always sends `Origin` (its own `ws://<host>:<port>` when none is given), so
  a server written on it cannot reach a listening node, and the library's own suite, whose fake
  servers are IXWebSocket clients, fails wherever one dials in (82 of its tests, which pass with
  the refusal alone taken out); two more assume the nursery's old refusal of a newcomer. No product
  bug: `aiosendspin`, Music Assistant's server, sends no `Origin`, and a server gets in beside a
  port scan's sockets held open (`test/system/sendspin_floods_test.cpp`).
- `cmake/sendspin-starved-floor.patch`: floors the playtime at now only when nothing is buffered
  (`starved_playtime_floor`), the spec's underrun recovery. Without it a playtime trailing now with
  frames still buffered is taken for an underrun: the library's open-loop playtime does so while ear
  defers its reports through a catch-up's burst ([above](#clock)). Kept with the deferral; whether
  it can go is a separate step, gated on evidence from both burst shapes.
- `cmake/sendspin-late-chunk.patch`: skips a chunk as too late only once its last frame is, so a
  chunk whose front is late is decoded and its front dropped by the hard sync. Without it every
  chunk whose start was late was skipped whole, overshooting the timeline by up to a chunk, which
  the next hard sync filled with silence. Both carry tests in the library's own suite
  (`PlayerHardSync.*`), which ear's build does not compile; the `sendspin` skill says how to run it.
  The late chunk's is also proven through ear: a network gap that starves the library (the test
  server holds the audio it sends past its own late check, then sends it at once, the release
  placed inside a chunk) is corrected in one step, the late chunk's front dropped, with no silence
  added; with the patch's change reverted the same test adds silence every run. The floor puts the
  next frame at now, and the block plays it from the first cycle to start after the library wrote
  it, so the stream comes back late by up to a block, or two when its decode outlasts the cycle it
  floored in, which the next chunk's sync may drop once more (a settle step).
- `cmake/ixwebsocket-limits.patch`: refuses frames over 1 MiB and messages over 1024 fragments;
  lets one transport log line through every 10 s; makes a socket still to upgrade give way (8 per
  address and 32 in all, both counting only sockets whose peer has not closed); keeps a close
  racing a peer's reset from ending the process; ends a connection whose socket closes while it is
  open (a stop landing as an upgrade was answered left its thread spinning, and the stop never
  returned).
- The library has no log hook: its lines (and IXWebSocket's) are a bare `fprintf(stderr, ...)`, so
  they reach standard output as unattributed `log` records from `stderr` at `warn`. ear fixes its
  level at WARN.
- The library has no retry after a lost dial (hence the connection watch) and only logs a port it
  cannot take (hence the check at start). `stop()` takes under 10 ms and the destructor 1 ms, even
  while dialling a dead address.
- One format, FLAC 24. *Why:* the library hands audio over with no format beside it, and a
  `stream/start` reaches the worker out of step with the audio, so a server changing depth mid-stream
  would turn it to noise if a second depth were listed.
- Opus is off: libsndfile pulls in `libopus.so`, and micro-opus's static C symbols would interpose on
  it.
- The library's clock correction averages two frames, at most once a chunk (4608 frames): adding a
  frame gives `n-1`, the blend of `n-1` and `n`, `n`; dropping one gives `n-1`, the blend of `n` and
  `n+1`, `n+2` (a step of 3). A recorded ramp is exact but for those, which the system tests' ramp
  check (`checkRamp`) allows and counts. At the low word's wrap the blends are not `k + 0.5`, because
  each channel is a signed sample.
- What Music Assistant does was read in its source (`providers/sendspin/{provider,player}.py`), never
  run: **pairing with a real Music Assistant has not been tested.** Every test runs against
  `aiosendspin`, Music Assistant's pinned server.

Not built: volume and mute, line sense, the editor showing notes and the code, IPv6, `wss://`,
several servers at once, choosing the advertised interface, RFC 6762 probing, more than stereo; a
pairing hold that survives a restart (the 20 rounds are counted in the library's memory, so any
restart, a crash a peer caused included, gives another 20 guesses at the 6-digit code). Not
planned: Opus, the protocol's other roles.

Not yet sent upstream: the `Origin` refusal; unproved sockets giving way, in the library and in
IXWebSocket; IXWebSocket's unbounded frames and fragments and its close racing a reset; connection
lost and dial failed events (which would retire the connection watch's timers); `on_audio_write`
carrying the chunk's format; a persisted round count; mjansson/mdns's `mdns_socket_listen` reading
a question's type and class past the buffer.

Not yet checked: a real Music Assistant (above); real hardware, and so a real card's latency in
the clock; realtime scheduling; a pod with a CPU limit (the library's unprioritised threads refill
a 43 ms feed); a real `fsGroup` mount; an off-link mDNS query from a real other subnet.
Operating it is also the `sendspin` skill's (`.claude/skills/sendspin/SKILL.md`), and running it in
a pod the `ear-on-talos` skill's.
