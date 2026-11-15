# AirPlay into ear: the `airplay` node type

**Status:** revision 3, system layer; both questions answered by the owner on 2026-11-14. Slice 1 (AirPlay 1, one node, tested end to end) is built; slices 2 (many speakers) and 3 (the AirPlay 2 setting, nqptp run by the operator) are not.
**Layer:** Domain · **System (this page: the concepts it adds, its parts, their public interfaces)** · Service (skipped: one process) · Module · Interfaces · Implementation

| # | Question | Kind | Owner's pick, 2026-11-14 (guidance) |
|---|---|---|---|
| Q1 | Should a node's audio join through PipeWire's pipe module (ear already tests it; fixed ~0.18 s delay) or ear's own tracked converter (not built; adjustable)? | general guidance | A, PipeWire's pipe module (built); T5's tracked mode waits for another first user |
| Q2 | May the AirPlay 2 setting ship with its sound checked by hand, since no scripted AirPlay 2 sender is known? | general guidance | ship, sound checked by hand: an exception to `docs/testing.md` for the AirPlay 2 sound path only (§8) |

## What was built (slice 1, the tracer)
Slice 1 is built and its material has moved into the library docs: the `airplay` node type on AirPlay 1
([`docs/node-types.md#airplay`](../node-types.md#airplay)), programs and `claim::AnyTcp`
([`docs/writing-a-node-type.md#programs`](../writing-a-node-type.md#programs)), pipe receivers
([`docs/engine.md#pipe-receivers`](../engine.md#pipe-receivers)), the per-node advertiser in `ear_mdns`
([`docs/architecture.md#packages`](../architecture.md#packages)), shairport-sync from source in the images
(`docker/Dockerfile`) and the scripted sender ([`docs/testing.md#the-airplay-sender`](../testing.md#the-airplay-sender)).
What follows is what slices 2 and 3 still need.

Your answers of 2026-11-14, all guidance: **A1** "different Airplay nodes in ear have no requirement to align with one another. they will maintain their own independent clocks, as all nodes do". **A2** "ear shouldnt know anything about kubernetes. So if there are aspects that cant be handled as pure subprocess, then ear cant do it. What about having an airplay1 node built in and also having instructions for how to configure an airplay2 node?" **A3** (follow-up): you picked "ear node, host runs nqptp" (worded by the agent): an AirPlay 2 setting, nqptp run by you from written instructions, ear only checking that nqptp is reachable.

## 2. What the owner has said (no absolute requirement binds AirPlay)
| # | Your words (guidance) | Date | Used for |
|---|---|---|---|
| G1 | "i will want multiple shairport instances" | 2026-10-29 | many nodes, one program each (slice 2) |
| G5 | "Backoff, never gives up"; a node failing "does not effect the graph globally" | 2026-11-07/08 | an exit fails its own node only |

| New term | Meaning | Could be |
|---|---|---|
| nqptp; channel | AirPlay 2's timing service, one per machine, yours; the shared-memory file it keeps per room | — |

## 4. What slices 2 and 3 add
- **Identity from the node:** the type gives shairport-sync its nqptp channel and device id, made from the node's name (unique in ear), so two rooms never share a channel. Today shairport-sync makes a device id per run, so a restarted speaker is new to senders.
- **Probing and renaming** in the advertiser (RFC 6762 §8-9): speaker names are human and can clash on the network. Today a listing is announced without probing.
- **Distinct UDP ranges** for many nodes: shairport-sync's AirPlay 1 ports start at 6001 for every node today.
- **AirPlay 2 is set outright,** never shairport-sync's `auto`, which falls back to AirPlay 1 silently; the pipe receiver at s32le, 48 kHz.

## 5. nqptp: "reachable", as events
```
configure ▶ claims ▶ pipe receiver running ▶ report feed open ▶ program starts ─┬ nqptp found ▶ listing ▶ announced ▶ running
                                                                                └ missing: exits ▶ start-failed "The NQPTP service can not be found. ..."
running ── nqptp exits, removing the room's channel ▶ file event ▶ program ended ▶ lost ▶ slice 8 retries
```
- **At start, the check is shairport-sync's own:** it asks nqptp for its channel before listing anything, and exits with that line if none comes.
- **While running, shairport-sync never notices** nqptp leaving: it reads the dead channel and stays silent. So a program can name files it needs (`needs`, §7), and their removal ends it; the type names its channel.
- **AirPlay 2 is set outright,** never shairport-sync's `auto`, which falls back to AirPlay 1 silently.

| Home | What the instructions must say |
|---|---|
| `docs/node-types.md#airplay` (host terms) | ear must be on the senders' network. For AirPlay 2: run nqptp at the commit `docker/Dockerfile` pins, on ear's machine, sharing its `/dev/shm`, with UDP 319/320 (low-port permission) and localhost UDP 9000; restarting nqptp restarts every AirPlay 2 room; the failure line |
| `ear-on-talos` skill (cluster terms) | the same beside ear's pod; cluster words live only here |

## 7. Public interfaces still to build (sketch)
```ts
interface AirplaySettings { airplay2?: boolean; }  // default false; true needs nqptp on the host
interface Advertiser {
  publish(listing: Listing): void; // probes the name, then announces; on a clash, renames ("Kitchen (2)")
  onAnnounced(handler: (publishedName: string) => void): void; // the type reports ready here
}
interface ProgramShape {
  needs?: SharedMemoryName[];  // removing one ends the program: the node fails "lost"
}
```

## 8. What happens next
| # | Slice (vertical, each lands green) | Needs | Delivers |
|---|---|---|---|
| 1 | Built: see [What was built](#what-was-built-slice-1-the-tracer) | | |
| 2 | Many speakers: identity from the node, probing and renaming; two nodes and a clash tested | 1 | G1 |
| 3 | The AirPlay 2 setting, `needs`, nqptp in the dev image for tests only, both instruction homes | 2, slice 8, Q2 | A2's "instructions for how to configure an airplay2 node"; A3 |

> **Q2. May the AirPlay 2 setting ship with its sound checked by hand?** *General guidance: which way should this lean?*
> <details><summary>Background</summary>
>
> | | **Ship it, sound checked by hand (recommended)** | Hold it for a scripted sender |
> |---|---|---|
> | Proven in CI | AirPlay 1 end to end; for AirPlay 2 the config, the listing (test-run nqptp [U: low ports in the dev container]), nqptp missing, channel removed, the same join at 48 kHz | all that, plus a sender's audio |
> | Costs | departs from `docs/testing.md`'s "Audio is verified here, never on real hardware" for this path | iPhone grouping waits; a spike on OwnTone [U] |
>
> Your answer sets whether slice 3 ships before a scripted AirPlay 2 sender exists.
> </details>

## 9. Costs, deferrals, prior art
- **Spawning traps:** SIGTERM is blocked in ear's threads and children inherit that; ear holds descriptors; ear is PID 1 and reaps.
- **About 0.18 s per room** with Q1's A, plus ear's downstream latency, uncompensated.
- **Ports outside ear's claims:** shairport-sync's AirPlay 1 UDP range; nqptp's 319, 320, 9000. An `rtp` node on one fails to start.
- **nqptp limits:** 16 AirPlay 2 node names per nqptp run; a room returns up to 60 s after nqptp does (recovery's start-failed wait); an nqptp killed without cleanup leaves its channel, undetected [U].
- **Young upstream, bigger image, open network:** shairport-sync's `metadata` mDNS hand-off landed 2026-11-01 (first in 5.6.3, 2026-11-10), in no Debian release, so ear builds from source. Discovery puts ear and port 7777 (no login) on the senders' network.

**Deferred:** track information, sender volume (slice 6), passwords, Home app pairing, remote control, 96/192 kHz, multichannel, arm64, lip-sync offset. **Not planned:** an in-process receiver; a container per room; ear starting nqptp. **Prior art:** shairport-sync 5.6.3 (used: the only maintained Linux receiver with AirPlay 2 and many instances); nqptp, PipeWire's pipe module (used); pyatv, PipeWire's raop-sink, OwnTone (test senders [U]); UxPlay, RPiPlay, shairplay (rejected: mirroring only, AirPlay 1 only, unmaintained).

## 10. Reconcile with existing code
| Where | Change | Cost |
|---|---|---|
| `ear_mdns`'s `Advertiser` | probing and renaming; `onAnnounced` | new behaviour, tests on real sockets |
| `ear_core`'s programs | `needs` (a file whose removal ends the program) | a file watch per program |
| `docker/Dockerfile`, `docs/node-types.md`, `ear-on-talos` | nqptp pinned, dev image only; shairport-sync with AirPlay 2; the instructions | image size, docs |

**For the code architects (parked):** a test pinning the pipe module's 8192 frames; the module never driving the graph (TM6), and its resync on bursts; ear's port probe racing the program's bind; node names over 62 characters mapped one-to-one to channels; a device id unique on the network and kept across restarts; AirPlay 2 pairing files.

*Not run for slices 2 and 3: no build, spike or test. nqptp 1.2.9 read from source; iPhone behaviour unverified [U].* *History: v1, v2 2026-11-14 (on-time join, pod helper); v3 the same day after your answers, revised after two reviews; slice 1 built 2026-11-15.*
