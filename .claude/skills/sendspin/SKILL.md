---
name: sendspin
description: The sendspin node in EAR, Music Assistant's audio in and out over the Sendspin protocol - dialling or listening per node, pairing by a code, the key file, mDNS, what each `says` line means, its threads and clock (the feed, the notifier, GraphCycle), the protocol library sendspin-cpp's traps, the carried Origin patch and bumping the pin, the real-server test tool, and the real Music Assistant check still owed. Use when configuring or pairing a sendspin node, when it is silent, is not found, keeps asking for a code, or fails with `cannot listen on port`, or when changing src/sendspin/, cmake/SendspinDeps.cmake, cmake/sendspin-origin.patch or test/support/sendspin_server.py.
---

# The `sendspin` node

The design and its evidence are `docs/design/sendspin.md`; settings and status lines are in `AGENTS.md`. This is how to
operate it, and what bites.

## Connecting and pairing

- **With `server` it dials; without, it listens and advertises `_sendspin._tcp`.** Never both: the spec forbids a client
  dialling while it advertises, and a dialling node opens no listening socket.
- **Pairing is required, for good.** Unpaired access is off, so a server that approves the node without pairing gets no
  roles: `says connected <server> unpaired`, and no audio. Start pairing in Music Assistant, read `says pairing-code
  <123-456>`, type it there. `says paired`, then `says connected <server> paired`, with no `disconnected` between. A wrong
  code keeps the same code showing.
- **20 wrong rounds hold pairing back** (`says pairing-failed pairing-code-mismatch`, `says pairing-held`), for this
  client's life: the count is in memory. Change a setting (`name`, and back) or restart ear to release it. Any network peer
  can burn these rounds (spec: not partitioned by server or address).
- **The same settings change nothing.** `node.update` with what the node already has is no restart, for a failed node too.
  To retry, change a setting or restart ear.
- **Identity.** `<config-dir>/sendspin-<node>.key`: the private key, the pairings, the output delay. Renaming a node makes a
  new identity, and adding a removed name back inherits the old one. To wipe one, delete the file and restart the node. A
  file it cannot trust fails the node, left unchanged: delete or fix it. Never log it or send it to the editor.

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
- **The feed is small on purpose**, about 43 ms: with 2.7 s a correction lands 2.7 s late.
- `loop()` needs a timer, because the inbox has no wake hook, and nothing tells of a lost connection or a failed dial. So a 10 ms
  poll and the connection watch stay inside the worker; they never act on a live attempt.
- **Opus is off.** libsndfile pulls in `libopus.so`, and micro-opus's static C symbols would interpose on it.
- Teardown: block gone, handoff drained, `close`: the worker says goodbye, stops and destroys the client, then posts `closed`.
  Never a wait on the main loop. A dial in flight cancels within a poll; a half-open inbound socket takes up to 3 s.

## The pin and the patch

- sendspin-cpp is pinned at a master commit, not a tag: `accept_inbound_connections` came after v0.9.5.
- `cmake/sendspin-origin.patch` is carried: the spec says a client MUST reject an upgrade carrying `Origin`, and the library's
  host server accepts it. Without it a web page speaking Noise could declare `pairing`, knock an idle server off and burn the
  20 rounds. The peer sees 101, then the close, as IXWebSocket has no earlier hook.
- **Bumping:** change the commit in `cmake/SendspinDeps.cmake`, rebase the patch (`git diff` in `.cache/deps/sendspin-src`), read
  the library's `cmake/host.cmake` for new dependencies. A patch that will not apply stops configure, and a tree without it stops
  it too. The pin holds the patch's SHA-256, so a changed patch fetches the tree again. To file upstream, not yet done: the
  `Origin` refusal; connection-lost and dial-failed events, which would delete the watch's timers.

## Tests

- `test/support/sendspin_server.py` is the real `aiosendspin` server, Music Assistant's pin, a command per stdin line and an event
  per stdout line (its header lists them). `sendspin_server.hpp` spawns it. Its identity and pairings persist with `--state`.
- **A stream started right after a role change joins the ramp late**, so `play` waits for the role.
- **Tests running side by side (`ctest --parallel`) share one network.** A browser would dial its neighbours' nodes, so `--discover` takes the
  instance to look for. Keep the default port 8928 out of every test but "a connect to it is refused".
- **A comma in a Catch filter means OR, and a filter that matches nothing exits 0.** A probe that filters on a name holding a
  comma can stay green having run nothing. CTest takes these names as they are. Keep `;` out of test names.
- The ramp: left is the low 16 bits of the frame index and right the high 16, so FR's meter is silent for its first 1.4 s.
- `mdns.h` and the library parse what any peer sends, unfuzzed, inside the audio process: a crash silences every node.

## NOT RUN: the real Music Assistant

Every test here runs against `aiosendspin`, not Music Assistant, and pairing was read in its source only. **The check, not yet
done by anyone:** a Music Assistant on the same network, with its Sendspin provider. A listening node appears by itself; start
pairing in its interface, type the code ear prints, then play to the player and hear the audio out of a routed card. Then a
dialling node the same way. Until someone has, a PR says NOT RUN. Also never run: real hardware's 1 ms bound
(`ear-on-talos`), amd64 before CI, realtime scheduling, a pod with a CPU limit, a one-shot mDNS querier.
