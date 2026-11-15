# AirPlay into ear: the `airplay` node type

**Status:** revision 3 for the owner's gate, 2026-11-14 (read-only run: no repo file changed).
**Layer:** Domain · **System (this page: the concepts it adds, its parts, their public interfaces)** · Service (skipped: one process) · Module · Interfaces · Implementation
**What you are asked:** agree the shape, then answer two questions. The shape: each `airplay` node runs its own shairport-sync as a subprocess. AirPlay 1 is built in and tested end to end. AirPlay 2 is a setting on the same node, with nqptp run by you.

| # | Question | Kind | Where |
|---|---|---|---|
| Q1 | Should a node's audio join through PipeWire's pipe module (ear already tests it; fixed ~0.18 s delay) or ear's own tracked converter (not built; adjustable)? | general guidance | §3 |
| Q2 | May the AirPlay 2 setting ship with its sound checked by hand, since no scripted AirPlay 2 sender is known? | general guidance | §8 |

## What changed since your last look
Your answers of 2026-11-14, all guidance: **A1** "different Airplay nodes in ear have no requirement to align with one another. they will maintain their own independent clocks, as all nodes do". **A2** "ear shouldnt know anything about kubernetes. So if there are aspects that cant be handled as pure subprocess, then ear cant do it. What about having an airplay1 node built in and also having instructions for how to configure an airplay2 node?" **A3** (follow-up): you picked "ear node, host runs nqptp" (worded by the agent): an AirPlay 2 setting, nqptp run by you from written instructions, ear only checking that nqptp is reachable. Q3: no preference.
- **Deleted:** holding the lead, the on-time target, slice 0's spike, shape B (its reason was alignment), every pod, `hostNetwork` and sidecar item, the speaker-name and UDP-range claims, nqptp in the shipped image.
- **The join got simpler:** v2 rejected PipeWire's pipe module only because it cannot follow the sender's clock; now it fits as it is (Q1).
- **How "reachable" is read (§5), correct me if wrong:** shairport-sync's own check at start, and ear hearing nqptp remove a room's channel while running. Both are events; ear never speaks nqptp's protocol.

## 1. Today
```
iPhone / Mac ──mDNS + AirPlay──▶ [missing] nothing in ear answers
ear: card · rtp · sendspin · file   [missing] a way for a node to run a program (node code may not fork)
                                    [missing] mDNS publishing any type can use (it lives inside sendspin)
```

## 2. What the owner has said (no absolute requirement binds AirPlay)
| # | Your words (guidance) | Date | Used for |
|---|---|---|---|
| G1 | "i will want multiple shairport instances" | 2026-10-29 | many nodes, one program each |
| G2 | "an "airplay node" could launch the subprocess and wire everything up magically" | 2026-10-29 | the program resource |
| G3 | "Pipe into a feed first" (a pick, worded by the agent) | 2026-11-07 | audio through a FIFO |
| G4 | "Nodes should not have direct access to stdout or stderr." | 2026-11-07 | program output into the node's log |
| G5 | "Backoff, never gives up"; a node failing "does not effect the graph globally" | 2026-11-07/08 | an exit fails its own node only |

| New term | Meaning | Could be |
|---|---|---|
| speaker | how one `airplay` node shows in a sender's list, under its speaker name | the node |
| listing | the mDNS entries that make a speaker findable (not ear's standard-output records) | — |
| advertiser | a node's mDNS publisher: probes its name, announces, withdraws | sendspin's `Advertiser`, moved |
| FIFO; report feed | a named pipe (one program writes, another reads); the one shairport-sync writes reports into | "Pipe into a feed first" |
| pipe receiver | the join: PipeWire's pipe module reading the audio FIFO into the node's inside bank | a module-run converter, like RTP's |
| session | a sender playing to the speaker (idle ⇄ playing), reported as notes, never the node's state | — |
| nqptp; channel | AirPlay 2's timing service, one per machine, yours; the shared-memory file it keeps per room | — |

## 3. The proposed shape
```
 sender ─AirPlay─▶ shairport-sync (one per node) ─ ─ channel (shared memory) ─ ─ nqptp (yours; AirPlay 2 only)
                     ▲ start/stop, exit  │ audio FIFO        │ report feed
 ear ┌─ node ────────┼──────────────────┼───────────────────┼─────────────────────┐
     │  program resource                ▼                   ▼                     │
     │                    pipe receiver (PipeWire)     airplay type: parses reports: sessions → notes,
     │                     └▶ inside bank ▶ out ─┐      listing → advertiser ─mDNS─▶ senders find "Kitchen"
     └───────────────────────────────────────────┼────────────────────────────────┘
        routes ▶ card · sendspin · file ◀────────┘
```
**Each node is a source with its own clock** (TM2, TM3): the sender's pace meets ear's tick in the node's converter (TM5), as `rtp`'s does. Rooms never line up and need not (A1).

> **Q1. Which join should a node's audio take?** *General guidance: which way should this lean?*
> <details><summary>Background</summary>
>
> | | **A: PipeWire's pipe module (recommended)** | B: ear's tracked converter (T5) |
> |---|---|---|
> | Gives | Built and tested: ear's module-run test loads it as a source; PipeWire follows the pace, as for `rtp` | a buffer ear sizes, tens of ms [U]; ear counts dropouts |
> | Costs | fixed 8192-frame buffer in PipeWire 1.4.2 (0.19 s at 44.1 kHz, 0.17 s at 48 kHz); dropouts only in PipeWire's log; a new companion kind on the wire | T5 built for it: an ear-made stream, a FIFO reader, a fill loop, tests |
> | You would see | ~0.15 s more delay than B; senders play ~2 s behind [U for AirPlay 2], so mainly video lip sync shows it | less delay |
> | Timing model | departs from TM8, the brief's wording of your 2026-11-13 answer ("AirPlay for tracked"): tracked waits for another first user | TM8 as written |
>
> Your answer changes only how slice 1 builds the pipe receiver. Your words that bear on it: "under 10ms is obviously idea[l]" (2026-10-29); for `rtp`, "If 100ms is the best rep can do, then so be it" (2026-10-30).
> </details>

## 4. Canonical shape
**Domain.** `airplay` is a **source** (TM2) with no inputs. Health means "the program is alive"; sessions are the type's reports. A stopped node keeps its configuration (`docs/architecture.md#stopped-versus-disabled`), so its speaker stays listed and plays nowhere. The framework gains:
- **Program resource** (G2; A2's "pure subprocess"), generic: it knows no program's protocol. The type names its executable as a constant in its code, never from the graph file.
- **An advertiser per node,** moved out of `sendspin` with `sendspin` onto it. Node types may not link each other, so a second consumer needs a shared home (altitude tree, step 2). One per node, as `sendspin` already shares port 5353 per node, so a failure stays its node's (G5). It gains probing and renaming: speaker names are human and can clash on the network.
- **Pipe receiver:** a new companion kind, so a wire change and rows in the engine's property table.
- **`anyTcp` claim:** ear picks a free port; many nodes need distinct ports nobody types (G1), found by mDNS.
- **Identity from the node:** the type gives shairport-sync its nqptp channel and device id, made from the node's name (unique in ear), so two rooms never share a channel.

**Patterns:** supervisor (Erlang/OTP one-for-one, slice 8's backoff); one process per instance; adapter (the converter companion); mDNS probe and rename (RFC 6762 §8-9); fail fast, judged by the consumer (§5). **Rejected:** shairport-sync's own responder (deprecated upstream "for security reasons"); Avahi (a second per-machine daemon, against A2).

The rest is recommended as shown: sender volume ignored until slice 6 gives nodes a level; a second sender takes over; a renamed speaker is new to senders.

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

## 6. Design rules (design choices, not requirements)
1. **The program owns AirPlay; ear owns the graph.** Health is the process alive; sessions and the listing are reports the type reads.
2. **One request, one job.** The type composes a program, a pipe receiver and an advertiser, and names no FIFO or port: the framework hands each across. The one name it makes is its nqptp channel, from the node's name.
3. **Per-host things have one owner.** Ports: ear's claims. Speaker names: the network, by probing. nqptp: you.

## 7. Public interfaces (sketch)
```ts
interface AirplaySettings {   // banks: out (public), fromAirplay (inside, the join's), as rtp's
  name?: string;              // speaker name; default: the node's name; renamed on a clash, reported as a note
  airplay2?: boolean;         // default false; true needs nqptp on the host
}
interface CreateOf {
  pipeReceiver(shape: PipeReceiverShape): PipeReceiver; // new companion kind
  program(shape: ProgramShape): void;                   // new; starts once the node's companions are ready
  advertiser(): Advertiser;                             // new; one per node, withdrawn at teardown
  claim(c: BuiltClaim | { kind: "anyTcp" }): Granted;   // built; anyTcp granted at the claims step
}
interface PipeReceiverShape {  // its fixed delay enters the latency plan once
  into: InsideInput<2>;
  format: "s16le" | "s32le";   // pinned per mode, never "auto"
  rate: Hz;                    // 44100 (AirPlay 1) or 48000 (AirPlay 2)
}
/** stdin null; stdout and stderr into the node's log only (G4); dies with ear and reaped by it; torn down
 *  with the configuration (a signal, then a kill at the close deadline), so node.stop leaves it running;
 *  claims released after reaping; no callback after teardown. */
interface ProgramShape {
  executable: string;          // a constant in the type's code; never from the graph file
  args: ProgramArg[];          // checked text | a Granted port | a receiver's FIFO | a runtime file
  config: RuntimeFile;         // beneath the config folder; removed with the configuration
  feed?: { onBytes(chunk: Bytes): void }; // the report feed: open before the program starts; on the node's loop
  needs?: SharedMemoryName[];  // removing one ends the program: the node fails "lost"
}
interface Advertiser {
  publish(listing: Listing): void; // probes the name, then announces; on a clash, renames ("Kitchen (2)")
  update(listing: Listing): void;  // new TXT entries, same name
  onAnnounced(handler: (publishedName: string) => void): void; // the type reports ready here
}
interface Listing {
  service: string;             // e.g. "_raop._tcp"
  name: string;
  port: Granted;
  txt: Record<string, string>;
}
type AirplayFailure =                            // existing codes only
  | { code: "start-failed"; detail: string }     // claim refused; exec failed; exited before announced (its last
                                                 // line); readiness deadline missed (a deadline only fails)
  | { code: "engine-refused"; detail: string }   // the pipe receiver never ran
  | { code: "lost"; detail: string };            // exited after announced; a needed file removed
```

## 8. What happens next
**Tracer bullet = slice 1:** one `airplay` node on AirPlay 1 in the dev image; a scripted sender (pyatv in a venv, as the sendspin server's tests run [U]) plays a tone. It proves an mDNS browse finds the speaker, a `file` node records the tone, and removing the node withdraws the listing and leaves no process or FIFO. Non-goals: AirPlay 2, clashes, Talos, an iPhone, volume, recovery.

| # | Slice (vertical, each lands green) | Needs | Delivers |
|---|---|---|---|
| 1 | Program resource; pipe receiver (wire, fixtures, editor in one commit); `anyTcp`; advertiser moved, `sendspin` onto it; `airplay` on AirPlay 1; shairport-sync from source in the image. **Tracer** | 3c (built), Q1 | A2's "airplay1 node built in", tested end to end |
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
| `companions.hpp`, `docs/control-port.md`, `test/protocol/`, `web/src/protocol.ts`, property table | `pipeReceiver` kind on the wire; the test's pipe-module shape becomes it, loaded through `loadModule` | a wire change, one commit |
| `ear_core` (interfaces), `ear_runtime` (spawning), a new mDNS package | program resource and advertiser behind core's interfaces; `Advertiser` and mjansson/mdns leave `sendspin` | a move, plus probing |
| `claims.hpp`, `KernelPortProbe` | a claim that assigns (`anyTcp`) | new claim behaviour |
| `programs.md`, `timing-model.md` TM8/T5, `docs/design/README.md` | 2.2 becomes the pipe receiver, 2.4 picks `advertise`; with A, T5 waits; README row rewritten | doc edits |
| `docker/Dockerfile`, `docs/node-types.md`, `ear-on-talos` | shairport-sync from source (no Avahi, D-Bus); nqptp pinned, dev image only; the instructions | image size, docs |

**For the code architects (parked):** a test pinning the pipe module's 8192 frames; the module never driving the graph (TM6), and its resync on bursts; who makes the report FIFO, and a reader that drains apart from the node loop (shairport-sync writes blocking); ear's port probe racing the program's bind; node names over 62 characters mapped one-to-one to channels; a device id unique on the network and kept across restarts; AirPlay 2 pairing files; `sendspin`'s goodbye timing on the moved advertiser; pidfd, environment allowlist, working directory, grandchildren.

## Review answered
- Accepted and changed: generic program and feed (§7); the claim key (identity from the node, §4); companion kind (§10); the advertiser's reason and alternatives (§4); `udpRange` dropped; stop (§4; the tracer now removes); tracer first (§8); nqptp out of the shipped image; mid-run nqptp loss (§5); port 9000; terms; the README row.
- "A shared library, not a framework resource": kept a resource, still one per node, so teardown withdraws by construction.
- "A readiness deadline is a timer": the event (announced) drives readiness; the deadline only fails a node that never announces, as companions' do.
- "48 kHz s32le makes shairport-sync resample": not shown harmful; the pin keeps both ends agreed; parked.

*Not run: no build, spike or test. PipeWire 1.4.2's pipe module, shairport-sync master and nqptp 1.2.9 read from source; iPhone behaviour unverified [U].* *History: v1, v2 2026-11-14 (on-time join, pod helper); v3 the same day after your answers, revised after two reviews.*
