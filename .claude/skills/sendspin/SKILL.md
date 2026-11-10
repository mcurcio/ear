---
name: sendspin
description: The sendspin node in EAR, Music Assistant's audio in and out over the Sendspin protocol - dialling or listening per node, pairing by a code, the key file, mDNS, what each `says` line means, its threads and clock (the feed, the notifier, GraphCycle), what the spike learned, the protocol library sendspin-cpp's traps, the carried patches and bumping the pin, trying it against the real-server test tool, and the real Music Assistant check still owed. Use when configuring or pairing a sendspin node, when it is silent, is not found, keeps asking for a code, or fails with `cannot listen on port`, or when changing src/nodes/sendspin/, cmake/SendspinDeps.cmake, cmake/sendspin-listener.patch, cmake/ixwebsocket-limits.patch or test/support/sendspin_server.py.
---

# The `sendspin` node

Its reference (settings, notes, failures, clock, threads, library facts) is `docs/node-types.md#sendspin`. This is how to
operate it, and what bites.

## Connecting and pairing

- **With `server` it dials; without, it listens and advertises `_sendspin._tcp`.** Never both: the spec forbids a client
  dialling while it advertises, and a dialling node opens no listening socket.
- **Pairing is required, for good.** Unpaired access is off, so a server that approves the node without pairing gets no
  roles: `says connected "<server>" unpaired`, and no audio. Start pairing in Music Assistant, read `says pairing-code
  <123-456>`, type it there. `says paired`, then `says connected "<server>" paired`, with no `disconnected` between. A wrong
  code keeps the same code showing.
- **20 wrong rounds hold pairing back** (`says pairing-failed pairing-code-mismatch`, `says pairing-held`), for this
  client's life: the count is in memory. `node.restart` (or changing a setting, or restarting ear) makes a new client and
  releases it; `node.stop` and `node.start` do not. Any network peer
  can burn these rounds (spec: not partitioned by server or address).
- **The code is on standard output only.** Notes never reach the control port or the editor (deferred), so whoever pairs
  needs ear's log open.
- **What Music Assistant does** (read in its `providers/sendspin/{provider,player}.py`, never run): `start_pin_pairing` for a
  client that `is_connected`, whichever side dialled, and a dynamic code when digits are offered. So a node must be connected
  before Music Assistant can offer pairing: `says connected "<server>" unpaired` comes first.
- **The same settings change nothing.** `node.update` with what the node already has is no restart, for a failed node too.
  To retry, `node.restart` or `node.start` (a failed node is configured again only when asked).
- **A server that dialled by hand may not come back after a restart of the node.** After a `node.update` or `node.restart`
  the goodbye is
  `shutdown`, and `aiosendspin`, having dialled by hand, did not redial within 30 s, though the spec has a server reconnect on
  that reason. One browsing mDNS comes back by itself.

## The identity file

- `<config-dir>/sendspin-<node>.key`, mode 0600, one JSON object with its audit fields (`createdAt`, `createdBy`, `changedAt`,
  `changedBy`) and the library's blobs in base64url: the private key, the pairing secrets, the output delay. At most 64 KiB, and
  written whole on each change, through `ConfinedFolder`, swapped in as `ear.conf` is. The keeper's watch ignores it. Never log
  it or send it to the editor.
- A wider mode is narrowed, not refused, at each boot for every key file in the folder and its staging file, and at each start
  and save, because the `fsGroup` that `ear-on-talos` prescribes widens it on every mount.
- Renaming a node makes a new identity, and adding a removed name back inherits the old one (`node.remove` leaves the file). To
  wipe one, delete the file and restart the node (`node.restart`): a file something else changed or removed fails the write
  (`key-write-failed`), so ear never puts it back.
- A file it cannot trust (not JSON, a missing field, a `keypair` of the wrong size, a symlink, too large) fails the node, left
  unchanged, mode included: delete or fix it. No new identity is minted over it, or the server's trust would be lost silently.
- No file at all and a folder it cannot write fails the node too, rather than run a fresh identity each boot.

## What a line means

| You see | It means |
|---|---|
| `running`, meters silent, `says dialling` | No server yet. Not a failure: the graph keeps running whatever the server does |
| `says disconnected` | `is_connected()` read false for 5 s (it reads false through a re-handshake, hence the 5 s). A dialling node redials then, and every 31 s until connected. A server back after the first redial can take up to 31 s to be heard |
| `failed start-failed: cannot listen on port <p>: held by node <n>` / `: Address already in use` | Another `sendspin` node, or another program. `node.update` to a free port. A program taking the port just after the check is not caught: the library only logs, and retries every 5 s |
| `says listening`, no `says connected` | Nobody dialled. See mDNS, below |
| `says advertise-failed <detail>` | Listening, not advertising. Give the server `ws://<address>:<port>/sendspin` |
| `says stream <codec> ...` and silence | A format ear did not ask for breaks the spec: the sink stays full, and the library closes the connection when its ring pins |
| `says key-write-failed <detail>` | Pairing holds this run only. Fix the folder, pair again. A key file something else changed is left as it is |

## Lifecycle: the legacy adapter until slice 3c

`sendspin` is the one type still written on the old type interface (`NodeTypeOf`, a config whose `start` returns a
`StartResult` with a recipe that builds the `RunObject`, its handoff asked through `StartContext`). Since slice 3a it runs
under the node lifecycle through the framework's legacy adapter (`src/core/legacy_instance.hpp`,
`docs/writing-a-node-type.md#the-old-interface`): its `start` is the configuration's `configure`, its recipe runs once every resource is ready,
its code is driven and faded like any other, and its `RunObject::close` runs off the main loop at the teardown. What
that means to an operator:

- **A stop keeps the connection.** `node.stop` fades and silences its code, and nothing else: the worker, the
  connection, the pairing and the library keep running (there is no `stop()` hook to say goodbye with). Its
  outputs are silent and nothing it is sent is played or reported played. `node.start` drives the code again; the feed
  is not cleared at a stop, so up to about 43 ms it held from before plays first (accepted until slice 3c,
  `docs/design/node-primitives.md`). It never reports the stop's gap as played (every stop prepares the code afresh).
- **A restart, a disable or a removal drops it.** The run object closes (goodbye `shutdown`, the worker joined) and a
  restart or an enable builds a new client from the key file. A disabled node holds no port and no connection.
- **Bugs fail, never crash.** A recipe that throws fails the node `start-failed` (`recipe threw: ...`), a `close` that
  throws is a `log` record, and the run past its close deadline (10 s) is leaked with `ear.close-overdue`.
- **It is not in the type harness** (`notYetInHarness` in `test/nodes/all_types.hpp`); its own tests and the system
  tests cover it. Slice 3c moves it onto the type's class (loops, a source and a sink instead of the feed and the
  handoff), and the adapter, `StartContext`, `RunObject` and `ThreadHandoffs` go with that step.

## mDNS

- `mjansson/mdns`, one header, in `advertiser.cpp`, on the worker. The library has no advertiser, and avahi needs a daemon and
  D-Bus. One socket per node, all sharing port 5353 with `SO_REUSEPORT`.
- **Answers go by multicast**, whatever the question's unicast bit: the kernel hands a unicast datagram to one socket of a
  shared port, so it could reach a neighbour's. Only a querier on another port than 5353 gets a unicast answer.
- **Limits, so it reflects nothing:** queries only (a response is ignored), from this link only, each answer once per datagram,
  by multicast once a second and by unicast at most 16 a second. A querier that asks again within the second is answered next
  time it asks.
- The instance is `ear-<first 50 bytes of node>-<first 8 of client id>`, a DNS label's 63 at most, stable and unique with no
  probing. Music Assistant shows `name`, from the hello.
- One interface, the one the kernel routes 224.0.0.251 through, and its address is read once at the node's start. A pod on an
  overlay network cannot advertise (`ear-on-talos`).
- Goodbye goes out when the worker leaves its loop. A crash leaves the record until its TTL.

## Threads and clock

- **The library never touches a realtime thread or PipeWire's main loop.** `notify_audio_played` and `write_audio` take
  mutexes on host; `stop()` and the destructor can wait. So: feed (network to audio), handoff (audio to network), mailboxes
  and a bell.
- **Audio time is the graph's.** The library is told when frames play from the cycle's start on the raw clock
  (`GraphCycle::startRawNs`, which the framework makes by sampling both clocks; the code reads no clock), never from when the
  thread woke. Wake time failed the spike: 52% within 1 ms against 100%.
- **A skipped cycle counts as played**, or it costs 25 ms: the library sees -5.3 ms, drops a chunk and inserts silence. A
  jump larger than the feed, or backward, is a change of driver (a card joining or lost), not a skip.
- **The feed is small on purpose**, about 43 ms at 48 kHz: with 2.7 s a correction lands 2.7 s late.
- **No card moves the clock** (`docs/engine.md#the-internal-clock`): the internal clock (the dummy driver)
  always drives and tracks the raw clock, and a linked card follows it. Before, a card joining sent the position from 2.3e10 to
  0 and losing it forward by 2.3e10; the code still treats a jump past the feed's capacity, or backward, as no skip, and
  `GraphCycle` now says `discontinuity` and `driverId` for it. 1 ms holds on the dummy alone and through a following test card
  (clockless, so no drift); the real card is a check in `ear-on-talos`.
- **The output's sync reaches the following card's buffer**, as far as the card reports it: the latency downstream of the
  `from` outputs (`GraphCycle::downstream`), which the card block's pass-through carries back from the card's playback node
  (one quantum plus its headroom; 256 frames on the test card), in the finish time. Only routed paths count: unrouted, or
  routed to a `file` node, it is 0. A resampler before the card and anything past the card are left to the output delay in
  Music Assistant, which the library keeps in the key file. The node declares its feed (0 to its size) as the latency of
  `from`. PipeWire 1.4.2 never tells a block a port's last link went, so after a card is lost the old latency stays until a
  new card reports (the `pipewire-engine` skill's latency trap).
- **One format, FLAC 24.** The library's sync thread gives ear audio with no format beside it, and a `stream/start` reaches the
  worker out of step with it, so a server changing depth mid-stream would turn it to noise if a second depth were listed.
- **A listening node on an untrusted network:** sockets that prove nothing give way to newcomers, so holding them keeps no server
  out (`docs/node-types.md#on-an-untrusted-network`). What a peer can still do: flood, which keeps servers out for as long as it lasts past
  some rate (measured on loopback: about 16k connects/s from 4 addresses, about 9.6k from the server's own; slower floods cost a server a
  few seconds), and only stopping it lets them in; and, at any rate, knock a server behind its own address off while it is still in its
  handshake, which costs it a reconnect. A
  peer that finishes the published Sentinel handshake and activates nothing does hold the node from an unpaired server, and a paired one
  that has never played (the spec's rule, item 2). Port 8928 and mDNS's 5353 are on every interface, with no address setting. Dial there
  or filter both if that matters. A dialling node has no listener.
- `loop()` needs a timer, because the inbox has no wake hook, and nothing tells of a lost connection or a failed dial. So a 10 ms
  poll and the connection watch stay inside the worker; they never act on a live attempt.
- **Opus is off.** libsndfile pulls in `libopus.so`, and micro-opus's static C symbols would interpose on it.
- Teardown: block gone, handoff drained, `close`: the worker says goodbye, stops and destroys the client, then posts `closed`.
  Never a wait on the main loop. A dial in flight cancels within a poll; a half-open inbound socket takes up to 3 s.

## What the spike learned

Thrown away; its numbers are in `docs/node-types.md#library-facts-and-carried-patches`, against the real `aiosendspin`, on the dummy driver.

- PCM 16, FLAC 16 and FLAC 24 at the graph frequency arrived exact (ear now lists FLAC 24 alone), and the server resamples other rates. The source role only
  activates once paired, and the server decides when it streams.
- Two clients in one process run side by side, which is what lets each node be its own.
- The library has no retry after a lost dial (hence the watch) and only logs a port it cannot take (hence the check in `start`).
- `stop()` takes under 10 ms and the destructor 1 ms, even while dialling a dead address. Ear fixes the library's log level at
  WARN, for noise.
- The library has no log hook: on a host build `SS_LOG*` is a bare `fprintf(stderr, ...)` with one process-wide level
  (`src/platform/logging.h`). Its lines (and IXWebSocket's) reach standard output as unattributed `log` records from
  `stderr` at `warn`, paced with every other line from `stderr`, never as the node's own. Naming the node needs a
  carried patch that routes `SS_LOG*` through a settable callback carrying the client (slice 3c,
  `docs/design/node-primitives.md`); none is carried yet.
- A correction is one frame per chunk at most, -0.084% over 150 ms against the spec's ±0.5%.

## The pin and the patch

- sendspin-cpp is pinned at a master commit, not a tag: `accept_inbound_connections` came after v0.9.5.
- `cmake/ixwebsocket-limits.patch` is carried: IXWebSocket buffers whatever a frame header claims, before any Sendspin message. It
  refuses a frame or a fragmented message over 1 MiB (the library's own limit, reachable only after reassembly) and over 1024
  fragments, and lets one transport log line through every 10 s. It also makes a socket still to upgrade give way to a newcomer
  (8 an address), since its four sockets closed the surplus at accept, and keeps a close racing a peer's reset from ending the
  process. An address's 8, and the room's 32, count only sockets their peers have not closed (a dup of each socket, polled at
  admission): one closed stays on the list until its own thread sees it, which on a loaded host outlasted an honest peer's
  quick reconnects and refused them, and let a burst of bare connects (closed at once, none able to give way) fill the room
  and refuse the upgrade after it; only the 64-thread bound counts every listed socket. It also ends a connection whose
  socket closes while it is OPEN: the server's `stop()` closes every listed connection, one is listed before its handshake
  ends, and a `close()` landing between the 101 and OPEN leaves only a close request in the pipe, on which the first poll
  closed the socket and nothing else, so the thread spun in OPEN without a syscall and `stop()` never joined it
  (`docs/node-types.md#library-facts-and-carried-patches`). Rebase it the same way, in `.cache/deps/ixwebsocket-src`; configure
  fetches the tree again when the patch changes.
- `cmake/sendspin-listener.patch` is carried: the spec says a client MUST reject an upgrade carrying `Origin`, and the library's
  host server accepts it. Without it a web page speaking Noise could declare `pairing`, knock an idle server off and burn the
  20 rounds. The peer sees 101, then the close, as IXWebSocket has no earlier hook. It also makes an unproved connection give way
  in the nursery, its own address's first, and lengthens the listen queue, whose 5 dropped a server's SYN under a burst.
- **Worker's `server_max_connections` is 32,** the room those two need to turn over in; it was the library's 4.
- **Bumping:** change the commit in `cmake/SendspinDeps.cmake`, rebase the patch (`git diff` in `.cache/deps/sendspin-src`), read
  the library's `cmake/host.cmake` for new dependencies. A patch that will not apply stops configure, and a tree without all of it (asked with
  `git apply --reverse --check`, `cmake/CarriedPatch.cmake`) stops it too, so a probe left in a patched line stops the next configure. The pin holds the patch's SHA-256, so a changed patch fetches the tree again. To file upstream, not yet done: the
  `Origin` refusal; connection-lost and dial-failed events, which would delete the watch's timers. The IXWebSocket limits are not
  reported either, the owner's decision (2026-11-06: "Don't report for now").

## Trying it without Music Assistant

In one `scripts/dev.sh bash`, after `scripts/dev.sh make debug`, with `/tmp/c/ear.conf` as:

```conf
graph_frequency 48000;
block_size 256;
node sendspin ma;
node file rec { path "ma.wav"; }
route ma:from.left rec:record.left;
route ma:from.right rec:record.right;
```

```bash
mkdir -p /tmp/r
out/ear --config-dir /tmp/c --recordings-dir /tmp/r --control-port 0 > /tmp/ear.log &
/opt/sendspin-server/bin/python test/support/sendspin_server.py --listen 0 --dial ws://127.0.0.1:8928/sendspin
```

The server reads commands from its standard input. On `connected <id>`, type `pair`; `/tmp/ear.log` then says `pairing-code
123-456`; type `code 123456`, then `play 2`, then `quit`. Stop ear with `kill %1`, and `/tmp/r/ma.wav` holds the ramp.

## Tests

- `test/support/sendspin_server.py` is the real `aiosendspin` server, Music Assistant's pin, a command per stdin line and an event
  per stdout line (its header lists them). `sendspin_server.hpp` spawns it. Its identity and pairings persist with `--state`.
- **`--hold-activate` stops a real server between its hellos and its first `server/activate`,** the one place a server can sit
  unadmitted. `activate-ended close` is what a socket giving way looks like from the server: no goodbye.
- **Every 127/8 address is this host's, so one test can be peers on many addresses** (`connectFrom` binds its source). Peers on 127.0.0.1 share the server's own.
- **`ChildProcess::waitForLine` finds a line an earlier wait already saw.** A second `says connected ... paired` needs `waitForNthLine`.
- **A stream started right after a role change joins the ramp late** (at frame 16608 of 72000, each run), so `play` waits for
  the role, by a 5 ms poll: `aiosendspin` has no event for it. Under load it can still join late, so the tests ask for 1 s of the 1.5 s. The encoder also holds back the stream's tail, so 68607 of 72000
  frames arrive.
- **The clock tests judge only cycles the host left alone.** Each cycle goes in a `CycleLedger`; a cycle is judged when it is
  clean, 3 s into the ramp and 1.5 s past the last that was not (or that held silence mid-ramp: the worker starved), and a
  test plays a 90 s stream until it has judged 750. Before
  playing they wait for the internal clock to reach the raw clock's rate over the clean cycles of two seconds, and fail naming
  it if it never does. A host that stalls makes them take longer, never fail.
- **Tests running side by side (`ctest --parallel`) share one network.** A browser would dial its neighbours' nodes, so `--discover` takes the
  instance to look for. Keep the default port 8928 out of every test but "a connect to it is refused". A test's listening port comes from below the kernel's
  outgoing range (`freeTcpPort`), or a neighbour's flood can hold it as a socket's near end.
- **Docker's Mac file share can lose a folder removed and made again under one name:** `unable to get current working directory`, or
  no permission to read a build directory's cache. Use a new name each time.
- **A probe that breaks closing the dup (`settleUpgrade`) hangs the `Origin` test** rather than failing it, as its read has no deadline.
- **A comma in a Catch filter means OR, and a filter that matches nothing exits 0.** A probe that filters on a name holding a
  comma can stay green having run nothing. CTest takes these names as they are. Keep `;` out of test names.
- The ramp: left is the low 16 bits of the frame index and right the high 16, so FR's meter is silent for its first 1.4 s.
- **A clock error past 100 us leaves a fractional frame in the WAV, without a bug.** The library averages two frames, at most once a chunk (4608 frames): `n-1`, the blend of `n-1` and `n`, `n` to add one, and `n-1`, the blend of `n` and `n+1`, `n+2` to drop one, a step of 3. `checkRamp` passes only those, and counts them in `blends`; a red prints them. To force some, add 150 us to the finish `SendspinCode::process` publishes, for instance every other 24000 frames. The averages at the low word's wrap are not `k + 0.5`, because each channel is a signed sample: left -0.5 at both `0x7FFF` and `0xFFFF`, and right `h + 0.5` at `0xFFFF` (`docs/node-types.md#library-facts-and-carried-patches`).
- `mdns.h` and the library parse what any peer sends, unfuzzed, inside the audio process: a crash silences every node.

## NOT RUN: the real Music Assistant

Every test here runs against `aiosendspin`, not Music Assistant, and pairing was read in its source only. **The check, not yet
done by anyone:** a Music Assistant on the same network, with its Sendspin provider. A listening node appears by itself; start
pairing in its interface, type the code ear prints, then play to the player and hear the audio out of a routed card. Then a
dialling node the same way. Until someone has, a PR says NOT RUN. Also never run: real hardware's 1 ms bound
(`ear-on-talos`), amd64 before CI, realtime scheduling, a pod with a CPU limit.
