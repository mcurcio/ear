---
name: sendspin
description: The sendspin node in EAR, Music Assistant's audio in and out over the Sendspin protocol - dialling or listening per node, pairing by a code, the key file, mDNS, what each `says` line means, its threads and clock (the feed, the notifier, GraphCycle), what the spike learned, the protocol library sendspin-cpp's traps, the carried patches and bumping the pin, trying it against the real-server test tool, and the real Music Assistant check still owed. Use when configuring or pairing a sendspin node, when it is silent, is not found, keeps asking for a code, or fails with `cannot listen on port`, or when changing src/sendspin/, cmake/SendspinDeps.cmake, cmake/sendspin-listener.patch, cmake/ixwebsocket-limits.patch or test/support/sendspin_server.py.
---

# The `sendspin` node

The design and its evidence are `docs/design/sendspin.md`; settings and status lines are in `AGENTS.md`. This is how to
operate it, and what bites.

## Connecting and pairing

- **With `server` it dials; without, it listens and advertises `_sendspin._tcp`.** Never both: the spec forbids a client
  dialling while it advertises, and a dialling node opens no listening socket.
- **Pairing is required, for good.** Unpaired access is off, so a server that approves the node without pairing gets no
  roles: `says connected "<server>" unpaired`, and no audio. Start pairing in Music Assistant, read `says pairing-code
  <123-456>`, type it there. `says paired`, then `says connected "<server>" paired`, with no `disconnected` between. A wrong
  code keeps the same code showing.
- **20 wrong rounds hold pairing back** (`says pairing-failed pairing-code-mismatch`, `says pairing-held`), for this
  client's life: the count is in memory. Change a setting (`name`, and back) or restart ear to release it. Any network peer
  can burn these rounds (spec: not partitioned by server or address).
- **The code is on standard output only.** Notes never reach the control port or the editor (deferred), so whoever pairs
  needs ear's log open.
- **What Music Assistant does** (read in its `providers/sendspin/{provider,player}.py`, never run): `start_pin_pairing` for a
  client that `is_connected`, whichever side dialled, and a dynamic code when digits are offered. So a node must be connected
  before Music Assistant can offer pairing: `says connected "<server>" unpaired` comes first.
- **The same settings change nothing.** `node.update` with what the node already has is no restart, for a failed node too.
  To retry, change a setting or restart ear.
- **A server that dialled by hand may not come back after a restart of the node.** After a `node.update` the goodbye is
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
  wipe one, delete the file and restart the node: a file something else changed or removed fails the write
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
- **Audio time is the graph's.** The library is told when frames play from the cycle's start (`GraphCycle::startNs`, moved onto
  the raw clock), never from when the thread woke. Wake time failed the spike: 52% within 1 ms against 100%.
- **A skipped cycle counts as played**, or it costs 25 ms: the library sees -5.3 ms, drops a chunk and inserts silence. A
  jump larger than the feed, or backward, is a change of driver (a card joining or lost), not a skip.
- **The feed is small on purpose**, about 43 ms at 48 kHz: with 2.7 s a correction lands 2.7 s late.
- **A card's clock.** Joining a card, the position goes from 2.3e10 to 0, and losing it forward by 2.3e10; both are outside the
  feed's capacity, so neither is a skip. **CI's test card cannot prove 1 ms:** it has no clock of its own and runs the graph about
  2.1% fast, past the library's ±0.5%, so its test holds the hard sync's bound (5 ms, asserted under 20) and the recovery.
  Only the dummy driver has shown 1 ms; the real card is a check in `ear-on-talos`.
- **The output's sync reaches the driving card's buffer**, as far as the card reports it (`position->clock.delay`, in the finish
  time). A second card, a resampler and anything past the port are left to the output delay in Music Assistant, which the library
  keeps in the key file. It has never run on a card that reports one: the test card's null PCM says 0.
- **One format, FLAC 24.** The library's sync thread gives ear audio with no format beside it, and a `stream/start` reaches the
  worker out of step with it, so a server changing depth mid-stream would turn it to noise if a second depth were listed.
- **A listening node on an untrusted network:** sockets that prove nothing give way to newcomers, so holding them keeps no server
  out (`docs/design/sendspin.md` section 11, item 6). What a peer can still do: flood, which keeps servers out for as long as it lasts past
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

Thrown away; its numbers are in `docs/design/sendspin.md` section 5, against the real `aiosendspin`, on the dummy driver.

- PCM 16, FLAC 16 and FLAC 24 at the graph frequency arrived exact (ear now lists FLAC 24 alone), and the server resamples other rates. The source role only
  activates once paired, and the server decides when it streams.
- Two clients in one process run side by side, which is what lets each node be its own.
- The library has no retry after a lost dial (hence the watch) and only logs a port it cannot take (hence the check in `start`).
- `stop()` takes under 10 ms and the destructor 1 ms, even while dialling a dead address. Ear fixes the library's log level at
  WARN, for noise.
- A correction is one frame per chunk at most, -0.084% over 150 ms against the spec's ±0.5%.

## The pin and the patch

- sendspin-cpp is pinned at a master commit, not a tag: `accept_inbound_connections` came after v0.9.5.
- `cmake/ixwebsocket-limits.patch` is carried: IXWebSocket buffers whatever a frame header claims, before any Sendspin message. It
  refuses a frame or a fragmented message over 1 MiB (the library's own limit, reachable only after reassembly) and over 1024
  fragments, and lets one transport log line through every 10 s. It also makes a socket still to upgrade give way to a newcomer
  (8 an address), since its four sockets closed the surplus at accept, and keeps a close racing a peer's reset from ending the
  process. Rebase it the same way, in `.cache/deps/ixwebsocket-src`.
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
route ma:from_server_FL rec:FL;
route ma:from_server_FR rec:FR;
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
  the role, by a 5 ms poll: `aiosendspin` has no event for it. The encoder also holds back the stream's tail, so 68607 of 72000
  frames arrive.
- **Tests running side by side (`ctest --parallel`) share one network.** A browser would dial its neighbours' nodes, so `--discover` takes the
  instance to look for. Keep the default port 8928 out of every test but "a connect to it is refused". A test's listening port comes from below the kernel's
  outgoing range (`freeTcpPort`), or a neighbour's flood can hold it as a socket's near end.
- **Docker's Mac file share can lose a folder removed and made again under one name:** `unable to get current working directory`, or
  no permission to read a build directory's cache. Use a new name each time.
- **A probe that breaks closing the dup (`settleUpgrade`) hangs the `Origin` test** rather than failing it, as its read has no deadline.
- **A comma in a Catch filter means OR, and a filter that matches nothing exits 0.** A probe that filters on a name holding a
  comma can stay green having run nothing. CTest takes these names as they are. Keep `;` out of test names.
- The ramp: left is the low 16 bits of the frame index and right the high 16, so FR's meter is silent for its first 1.4 s.
- `mdns.h` and the library parse what any peer sends, unfuzzed, inside the audio process: a crash silences every node.

## NOT RUN: the real Music Assistant

Every test here runs against `aiosendspin`, not Music Assistant, and pairing was read in its source only. **The check, not yet
done by anyone:** a Music Assistant on the same network, with its Sendspin provider. A listening node appears by itself; start
pairing in its interface, type the code ear prints, then play to the player and hear the audio out of a routed card. Then a
dialling node the same way. Until someone has, a PR says NOT RUN. Also never run: real hardware's 1 ms bound
(`ear-on-talos`), amd64 before CI, realtime scheduling, a pod with a CPU limit.
