---
name: sendspin
description: The sendspin node in EAR, Music Assistant's audio in and out over the Sendspin protocol - dialling or listening per node, pairing by a code, the key file, mDNS, what each `says` line means, its threads and clock (the feed, the notifier, GraphCycle), what the spike learned, the protocol library sendspin-cpp's traps, the carried patches and bumping the pin, trying it against the real-server test tool, and the real Music Assistant check still owed. Use when configuring or pairing a sendspin node, when it is silent, is not found, keeps asking for a code, or fails with `cannot listen on port`, or when changing src/nodes/sendspin/, cmake/SendspinDeps.cmake, cmake/sendspin-listener.patch, cmake/sendspin-starved-floor.patch, cmake/sendspin-late-chunk.patch, cmake/ixwebsocket-limits.patch or test/support/sendspin_server.py.
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
- **The node's clock is an honest sink with a local skip** (`docs/node-types.md#clock`, the timing model's rule 3). It plays
  every frame in order. On a cycle whose facts say `catchUp` or `absent` (a pause) it skips the jump as far as the feed
  holds it, on that cycle, overdue or not, counted played; any other cause (a driver change, a jump back, a drive, the
  cycle after an oversized one) is a new clock, and a clearing cycle skips nothing of the next stream. Not counting a skip
  costs 25 ms (the library sees -5.3 ms, drops a chunk and inserts silence). **Never tell the library a frame ear did not
  take from what it wrote**: it subtracts what it is told from what it wrote and clamps at zero with a `Buffered frames
  underflow` warning (`sync_task.cpp`, `process_playback_progress`). What the feed did not hold of a jump is the
  library's: it sees it in the next finish it is told and corrects once at its next chunk's sync. No debt, no model of
  the library: the earlier wrapper (C4, 339149bf53 to a96f841750) owed that remainder from the library's next writes and kept `LibraryView` to know when it
  could, and both correcting the same lost time was the bug class it existed to fence off.
- **Told only from a cycle that began on time** (`GraphCycle::overdueNs` 0), its finish dated by that cycle's start: the
  driver catches a stall up in a burst of timeouts, of which the block runs as many as the host lets it
  (`docs/engine.md#the-internal-clock`: one catch-up of the whole stall on the dev host today, every other timeout once;
  every one, losing nothing, on a GitHub runner), and a burst's cycle is dated neither by its nominal start (behind now)
  nor by its run time (not where the next frame plays while the burst runs) truly. Told either, the library corrects
  where nothing was lost: the spike's first design, every overdue cycle told by its run time, makes two corrections and
  silence in the runner's uncut burst. `untoldBound` (2 s of the graph's nominal time, the cycles' `startRawNs`) is the
  deferral's only bound: past it the next cycle tells even if overdue, dated by `GraphCycle::ranRawNs()`, and the report
  after it is clamped so it never steps back (the clamp's only case; `clamped()` is the test seam). While ear defers, the
  library's playtime runs open loop and trails now, so the floor patch stays with the deferral (below).
  Before cycle facts the code worked the skip out from the position itself, and took a stall past the feed for a change
  of driver.
- **The feed is small on purpose**, about 43 ms at 48 kHz: with 2.7 s a correction lands 2.7 s late.
- **No card moves the clock** (`docs/engine.md#the-internal-clock`): the internal clock (the dummy driver)
  always drives and tracks the raw clock, and a linked card follows it. Before, a card joining sent the position from 2.3e10 to
  0 and losing it forward by 2.3e10; a driver change is a new clock to the code (`GraphCycle::facts` says
  `driverChange`), never a skip. 1 ms holds on the dummy alone and through a following test card
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

## The pin and the patches

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
- `cmake/sendspin-starved-floor.patch` is carried (the owner's decision when C4 was built, b8e359689e, kept by the timing model with
  the deferral): `starved_playtime_floor` moved the playtime up to now whenever it trailed now by more than 5 ms, taking
  that for an underrun, but it also trails now with frames still buffered: while ear tells nothing through a catch-up's
  burst the library keeps its playtime open loop (the last finish plus what it wrote since), behind now by the graph's
  lag. Floored mid-catch-up, the library took the next chunk for 37 to 67 ms late or early and corrected twice. The
  patch floors only when nothing is buffered, the spec's underrun recovery ("Large errors and startup"). Dropping it is
  a separate step, gated on evidence from both burst shapes without it (`docs/design/timing-model.md` 6.5).
- `cmake/sendspin-late-chunk.patch` is carried (same decision): `decode_chunk` skipped a chunk whenever its start was over
  5 ms late, so a catch-up of 154 ms skipped two 96 ms chunks, overshot by 37 ms and filled that with silence: two steps.
  The patch decodes first and skips only a chunk whose last frame is late (`SyncTask::chunk_too_late`); the hard sync
  drops the late front of the next, one step, no silence. It is the path the library's one correction after a stall past
  the feed takes, as after a network gap: both are the library's own behaviour made right for any sink.
- **Each patch has a deterministic test in the library's suite**, ear's build compiles none of them: `PlayerHardSync.
  AnEstimateWithFramesBufferedIsNotFloored` (the floor), `AChunkIsTooLateOnlyOnceItsLastFrameIs` and
  `AChunkLateAtItsStartGetsItsFrontDropped` (the late chunk). Reverting either patch's change alone fails its tests;
  through ear, see "Measured without the floor patch" below for the floor's. The late chunk's is proven through ear by
  the clock test "A starving network gap": the test server's `gap <ms>` holds the audio it sends past the server's own late check until the
  library has starved, then sends it at once with the release placed inside a chunk, and with the patch's change
  reverted the library skips that chunk whole and adds silence (5 runs of 5). Run
  the library's suite on the patched tree, in the dev image, after a configure has fetched and patched it:

  ```bash
  scripts/dev.sh cmake -S .cache/deps/sendspin-src -B out/sendspin-lib-tests -DSENDSPIN_BUILD_TESTS=ON \
      -DSENDSPIN_ENABLE_OPUS=OFF -DCMAKE_BUILD_TYPE=Debug   # Opus off: the image has no `patch` for micro-opus
  scripts/dev.sh cmake --build out/sendspin-lib-tests --parallel 4
  scripts/dev.sh out/sendspin-lib-tests/tests/sendspin_tests --gtest_filter='PlayerHardSync.*'
  scripts/dev.sh ctest --test-dir out/sendspin-lib-tests   # serially: its lifecycle tests bind fixed ports
  ```

  The library's build fetches its own dependencies and its examples' (FTXUI): with no network in the container, add
  `-DBUILD_EXAMPLES=OFF` and point `FETCHCONTENT_SOURCE_DIR_<NAME>` (`ARDUINOJSON`, `MICRO_FLAC`, `NOISE_C`,
  `IXWEBSOCKET`, `GOOGLETEST`) at trees fetched on the host.

  At 7be2278 with the three patches, 84 of the 133 tests in ConnectionLifecycle, EncryptedLifecycle,
  RoleDeactivation, ClientLifecycle, Start, SourceWire, VisualizerDelivery and Allocations do not pass (81 hang to the
  suite's watchdog, 3 fail at once), whichever IXWebSocket; pristine, all 133 pass, and every player test passes
  either way. **They are the listener patch's, and test assumptions, not bugs** (measured 2026-11-13, each test alone):
  - **82 are the `Origin` refusal.** IXWebSocket's client always sends `Origin` (`IXWebSocketHandshake.cpp`: its own
    `ws://<host>:<port>` when none is given, a given one as given, so it cannot leave it out), and every fake server
    that dials the client in the suite is an IXWebSocket client, so each is closed after its 101 and the test waits
    for a connection that never comes. `ConnectionLifecycle.JunkProbeDoesNotBlockRealServer` is one: its probes are
    not what keeps its server out. With only the refusal taken out of the patch (displacement and backlog kept), these
    82 pass, and the whole suite on ear's trees (the three patches, ear's IXWebSocket) passes but for the two below, 736 of 738;
    with only the refusal added to pristine, the 82 fail again.
  - **2 assume upstream's full nursery refuses the newcomer.** `ConnectionLifecycle.FullNurseryOfLivePeersRejectsNewcomer`
    (two Sentinel peers that never activate must keep a paired newcomer out; under the patch one gives way, so the
    newcomer never closes and it hangs) and `ClientLifecycle.AnAcceptBehindADisconnectFindsTheNurserySlotsItFreed`
    (its control row expects a full nursery of unproved stand-ins to refuse). That is what the patch changes, on
    purpose (`docs/node-types.md#on-an-untrusted-network`).

  The real scenario, a port scan's two connects held open and then a real server, is ear's system test `A server gets
  in beside two bare sockets held open from its own address` (`aiosendspin` sends no `Origin`).
- **Each rule of the clock has a revert proof** (2026-11-13, at T2): the skip taken out (nothing skipped) fails six code
  rows, the hiccup clock test (the library corrects the block it was told late, twice) and the steady dummy-driver test;
  the deferral taken out (overdue cycles told, dated by their run time) fails the burst, overdue, bound and clamp-idle
  rows; the bound taken out (never told from an overdue cycle) fails the bound's row; the clamp made unconditional fails
  the clamp-idle row (a shrinking downstream latency is told as it is); the clamp taken out fails the bound's row.
- **What the stall tests measured on the dev host** (20 runs each beside four busy threads on four cores): every burst
  was cut, one catch-up of the whole stall, never an overdue cycle, so the uncut shape has not run here. The library's
  correction was the stall's lost frames less the feed's 2048, plus the zeros ear played (0 to 512), within 4 frames;
  a 200 ms stall is about 8700 frames, a 1 s one about 52000 upstream and 46000 in the block; a 1.5-cycle hiccup was
  skipped whole and corrected nothing. Under heavier load (another suite beside them, load 8 to 10) the library once
  followed its correction with a settle step of one block and once of two, neither with a dry or empty-feed cycle
  between them (the test now fails such a step, listing the cycles between), and host-starved runs failed the run's 1%
  blend bound; a timing test that fails on a loaded host is rerun
  alone before the code is suspected.
- **Measured without the floor patch** (upstream's floor put back in the kept tree, 5 runs of each stall test, same
  load): 20 of 20 passed, the same single corrections, because every burst here was cut into one catch-up told on time,
  so the library's playtime never ran open loop behind now. That says nothing of the uncut shape, the one the patch is
  kept for; dropping it waits for CI's runs of that shape without it.
- **The stall tests once took the host's burst for granted** and stalled inside the node's own block: on a GitHub runner
  the block then ran every cycle of the burst, nothing was lost, and the test failed finding no catch-up after the stall
  (`end > stalled + 1`, 1183 > 1183). A stall a test needs lost must be made upstream of the block that must lose it, and
  the burst's cycles cut on purpose (`StallProbe`, `docs/testing.md#a-writers-own-event`). The in-block stall tests now
  judge whichever shape they get, each by its own rule, and record it (`sendspin stall judged: ...` in
  `out/Testing/sendspin-stall-shapes.txt`); CI's `test` job prints that file, pass or fail. **The uncut shape runs only
  on a GitHub runner**: read the lines there after a change to the clock.
- **Draft upstream report** (not filed; the owner sends it):

  > **Title:** After a sink's catch-up, the sync task floors a playtime that is only behind, and skips whole chunks past
  > the timeline
  >
  > A sink that stalls and then catches up (a graph that runs its missed cycles back to back) cannot date a report
  > truly until it has caught up, so it reports nothing through the catch-up and leaves the sync task's playtime open
  > loop, which then trails the wall clock by the sink's lag with frames still buffered. Two things in
  > `sync_task.cpp` then move the audio off the timeline. (1) `starved_playtime_floor` moves
  > `new_audio_client_playtime` up to now whenever it trails now by more than `HARD_SYNC_THRESHOLD_US`, even with frames
  > still buffered; the spec's recovery is for an underrun, and with `buffered_frames > 0` the sink has not starved. The
  > next chunk is then judged tens of milliseconds late or early and corrected, and corrected back at the chunk after. (2)
  > `decode_chunk` skips a chunk whenever its start is more than the threshold late, so a sink late by 1.6 chunks skips
  > two and is then early by 0.4 of one, which the next hard sync fills with silence. Proposed: floor only when
  > `buffered_frames == 0`; decode first and skip only a chunk whose last frame is late, letting the hard sync drop the
  > late front of the next. Patches against 7be2278 attached, each with a deterministic test in `test_player_role.cpp`
  > (`PlayerHardSync.AnEstimateWithFramesBufferedIsNotFloored`, `AChunkIsTooLateOnlyOnceItsLastFrameIs`,
  > `AChunkLateAtItsStartGetsItsFrontDropped`). One cost to weigh on small targets: (2) decodes a chunk before it can know
  > the chunk's length, so a chunk wholly too late is now decoded and thrown away where it was skipped undecoded; on an
  > ESP32 a FLAC chunk's decode is milliseconds of the sync task's time. A chunk's frame count from its header, or
  > skipping undecoded when its start is late by more than the longest chunk, would keep the old cost for those.
  > Measured with the real `aiosendspin` server behind a sink that stalled 200 ms: before, two corrections or silence
  > in 11 runs of 14 (a sink that reported through the catch-up, dated behind now: the same floor fired).
- **Draft upstream report for the listener patch** (not filed; the owner sends it):

  > **Title:** The host server accepts an upgrade carrying `Origin`, and a full nursery of unproved peers keeps every
  > server out
  >
  > (1) connection.md says a client MUST reject a WebSocket upgrade carrying `Origin`; `ws_server.cpp` accepts it, so a
  > web page speaking Noise can declare `pairing`, knock an idle server off and spend the pairing rounds. Proposed: close
  > such an upgrade on `Open` (IXWebSocket has no earlier hook, so the peer sees 101, then the close). **Your test
  > fixtures will need changing with it:** IXWebSocket's client always sends `Origin` (`IXWebSocketHandshake.cpp`
  > adds `ws://<host>:<port>` when none is given), and every fake server that dials the client is one, so with the
  > refusal alone 82 tests in ConnectionLifecycle, EncryptedLifecycle, RoleDeactivation, ClientLifecycle, Start,
  > SourceWire, VisualizerDelivery and Allocations wait for a connection that never comes (81 hang to the watchdog,
  > `JunkProbeDoesNotBlockRealServer` among them; 3 fail). The fixtures need a client that can send no `Origin`. (2)
  > `ConnectionManager::accept` refuses a newcomer when the inbound nursery is full, so two peers that finish the
  > Sentinel handshake and never activate, renewed before the establish reap, keep every server out. Proposed: an
  > occupant that has proved nothing gives way, the newcomer's own address's first, then one with its Noise handshake
  > unfinished, then the oldest; only an operational one or one on a long-term PSK never does. This reverses
  > `ConnectionLifecycle.FullNurseryOfLivePeersRejectsNewcomer` (a paired newcomer now displaces a Sentinel mute) and the
  > control row of `ClientLifecycle.AnAcceptBehindADisconnectFindsTheNurserySlotsItFreed`. Also proposed: a listen
  > backlog of twice `max_connections`, as IXWebSocket's 5 dropped a server's SYN under a burst. Patch against 7be2278
  > attached.
- **Bumping:** change the commit in `cmake/SendspinDeps.cmake`, rebase the three patches in order (each `git diff` in `.cache/deps/sendspin-src` against the tree with the ones before it applied), read
  the library's `cmake/host.cmake` for new dependencies. A patch that will not apply stops configure, and a tree without all of it (asked with
  `git apply --reverse --check`, `cmake/CarriedPatch.cmake`) stops it too, so a probe left in a patched line stops the next configure. The pin holds the patch's SHA-256, so a changed patch fetches the tree again. To file upstream, not yet done: the
  `Origin` refusal and the nursery's displacement (drafted above); connection-lost and dial-failed events, which would delete the watch's timers. The IXWebSocket limits are not
  reported either, the owner's decision (2026-11-06: "Don't report for now").

## Trying it without Music Assistant

In one `scripts/dev.sh bash`, after `scripts/dev.sh make debug`, with `/tmp/c/ear.conf` as:

```conf
graph {
    rate 48khz;
    block_size 256;
}
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
  the role, by a 5 ms poll: `aiosendspin` has no event for it. Under load it can still join late, by however much the host delayed
  it, so a system test plays a 60 s stream until the WAV holds the second of exact ramp it needs (`waitForRamp`), then ends it
  (`end`, then `says stream-ended`), never a fixed length (`docs/testing.md#waiting-for-what-ear-wrote`). A stream that plays
  out ends short too: the encoder holds back its tail, so 68607 of 72000 frames arrive.
- **The clock tests judge every cycle through the one cycle judge** (`docs/testing.md#the-cycle-judge`). A cycle's timing
  is judged when the judge judged it whole, 3 s into the ramp, outside a library settle and a loss of sync the library
  logged, and a test plays a 90 s stream until it has judged 750; the steady tests make the judge's four 1.5-cycle
  stalls upstream of the node meanwhile, and judge the next whole cycle after each (the feed holds each, and ear skips
  it exactly). A library settle is declared only where the library realigns itself (a dry cycle, by the feed's own
  count `dryReads()`; a catch-up's jump the feed did not hold whole, `unskipped`; a hard sync or loss it logged), two
  of its chunks long. Before playing they wait for the internal clock to reach the raw clock's rate over the whole cycles of two
  seconds, and fail naming it if it never does. Every step and stretch of silence is accounted for by the code's skips,
  worked out from each cycle's facts, what the feed held (`lastHeldAtStart()`, checked against `lastSkipped()`) and
  what it lacked (`dryFrames()`), by a
  settle's evidence, or by the library's own lines, read from standard error at verbose
  (`docs/testing.md#the-librarys-own-word`): `Hard sync: dropping/adding <n> frames`, and whole late chunks it skips
  unlogged. A skip that takes the library's soft-sync frame with it moves the ramp one less (an insert) or one more (a
  drop) than its frames.
- **The source's stamps are judged step by step:** the server says each chunk (`source-chunk <frames> <stamp> <lag>`); each
  stamp must step by the last chunk's length within 200 us, or by a block or more where the graph skipped cycles, each
  skip paid by one of ear's counter records that covers it in time (`docs/testing.md#the-counters-own-word`), until a
  second of frames came on clean steps and one chunk arrived within 250 ms of its stamp. A ramp's skips are paid the
  same way.
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
