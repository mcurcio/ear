# EAR on PipeWire: system design record

Layer: system design (domain model and public interfaces). Agreed 2026-10-29 in chat and on the
design page, https://claude.ai/artifact/Ct6C2EPnD2S49oAeWmVNXZ (version 4), which holds the full
interfaces, diagrams and flows. This file records what was agreed. Module-level designs have their
own pages and gates: milestone 1 is `docs/design/m1-white-to-file.md`.

## Requirements (the owner's words, 2026-10-29)

| # | Requirement |
|---|---|
| R1 | "deliver some of the channels to one hardware device, some other combination of channels to a second hardware device, and then also have various network protocol sinks" |
| R2 | "in a general purpose sources-and-sinks graph, its always just 'running'" |
| R3 | Outputs on different devices stay aligned, for "seperate zones" |
| R4 | "under 10ms is obviously ideal" |
| R5 | Endpoints with a configurable rate "should be proactively configured to match the 'graph frequency'" |
| R6 | PipeWire should "handle low-level audio concerns so that `ear` can focus on the graph and UX" |
| R7 | "what matters is the _framework_ that ear provides and the graph ideology" |
| R8 | "ear will likely run on linux, inside a container, in my talos kubernetes cluster" |
| R9 | "i will want multiple shairport instances"; AirPlay 1 vs 2 "can be decided later" |
| R10 | "im fine with hardcoding devices and graphs"; hotplug "is not my main concern" |
| R11 | "can i add my own sources and sinks?"; "the core will be \"built into ear\"" |

## Answers (current choices; any can be revisited)

- Alignment is for separate zones: PipeWire's drift correction is enough.
- Live edits need not survive a restart "right now": `ear.json` is what loads at boot.
- Node types are built into EAR; nodes as separate programs may come later.
- Nodes, not only routes, can be added, changed and removed while EAR runs.
- An `airplay` node launches shairport-sync and wires it in; separate containers are the fallback.
- EAR is the PipeWire instance: one process runs PipeWire's engine and serves its socket.
- A program that keeps crashing restarts with a growing wait ("backoff sounds good").

## Shape

EAR is one process in one container. It runs PipeWire's engine, hosts the built-in node types,
serves the control port (7777), and serves PipeWire's socket so child programs (shairport-sync)
connect to it. PipeWire owns the clock, driver choice, rate matching, resampling and device and
network audio. EAR owns policy: which nodes exist and which links join them.

| Context | Owns | Never does |
|---|---|---|
| Control surface | Loading `ear.json` at boot by replaying it as control calls; the control port | Graph rules; engine calls |
| Desired graph | Wanted nodes, routes and graph frequency; route checks against declared ports | Knowing what exists |
| Node catalogue | The built-in node types | Deciding when nodes exist |
| Node framework | Each node's one state machine (starting, running, recovering, failed, stopping), from facts its type reports; its port block and inside links | Routes |
| Program supervisor | Starting programs, their EAR-issued identity, whether each is alive (start to exit event) | What a program is for |
| Reconciler | Route links and each route's status | Node state; audio |
| Engine (PipeWire) | Live objects, clock, rate matching, device and network audio | Policy |

## Design rules (design choices, not requirements)

1. Every piece of state has one owner, and each health judgement belongs to its own owner: a
   program is alive from start to exit; a node's state comes from the framework; a playing session
   is shown separately and never counts as health.
2. A node is self-contained: its type reaches PipeWire only through powers the framework grants
   for what the type declared; everything it makes carries its mark and goes when the node does.
3. The graph always runs (R2): every port block processes whether routed or not, nothing is
   suspended, and a node's insides coming and going never touch its port block or routes.

## Costs

- EAR is in the audio path: a crash, restart or upgrade silences every output until Kubernetes
  restarts it, and live edits since boot are lost.
- EAR supervises shairport, repeating part of Kubernetes' job.
- The runtime is rewritten: the JUCE graph, device code, pipe reader and their tests leave.
- EAR no longer runs on macOS; development there runs it in a Linux container.
- Zones, not sample-exact; the control port has no authentication yet.

Deferred: nodes as separate programs, network sinks, plugin processors, persisting live edits,
control-port authentication, several AirPlay 2 instances, changing graph frequency live.
Not planned: hotplug, sample-exact alignment, WirePlumber.

## Work outline

Milestone 1 (white noise into a file sink, through the in-process engine) landed on 2026-10-29
on branch `claude/m1-white-to-file`; its page is `docs/design/m1-white-to-file.md`. It changed
the order below:

- Step 9 came first: the JUCE runtime and its tests were removed in milestone 1's first slice,
  the build base (the owner's answer on the milestone 1 gate page, 2026-10-29: remove JUCE in
  this milestone). The docs and skills were rewritten at the milestone's end.
- Done: step 1 (Debian 13 image for development and CI, Linux CI); step 2; step 4; step 5 with
  the `white` and `file` types in place of null-sink and tone; step 3 without the socket or
  marks (no other client can connect, so EAR recognises its nodes by `node.name`); step 6's
  `ear.json` v2 only.
- Not started: the control port (rest of step 6), steps 7 and 8. The parked list is section 8
  of the milestone 1 page.

The original outline:

1. Build base: PipeWire libraries, a Linux dev and test container, Linux CI.
2. Pure core: graph spec, checks, errors, the node state machine.
3. Engine host: PipeWire inside EAR, its socket, marks, live objects.
4. Reconciler: route links, status and events.
5. Node framework plus the null-sink and tone types.
6. Control surface: `ear.json` v2 and the control port.
7. Program supervisor and the airplay type; a smoke test with real shairport-sync.
8. Sound card output; a check on the Talos hardware, including measured latency (R4).
9. Remove the JUCE runtime; rewrite the docs and skills.

The tracer test runs in a Linux container with no hardware, through `ear.json` and the control
port: tones routed to two virtual sinks arrive on their channels with every other channel exactly
zero; an airplay node with a stand-in that behaves like shairport is running while idle and plays
while "played"; a crash recovers without unlinking the route; a bad route returns `unknown-port`;
removing the node silences its channel. Milestone 1's system tests cover part of it through
`ear.json` alone: white noise routed to one channel of a file sink, the other channel exactly
zero, and a bad route ending EAR with `unknown-port`.

Tested in milestone 1 (`src/pipewire/tests/engine_test.cpp`, PipeWire 1.4.2): an output-only
node acting as a source, and node activity with no session manager. Still to test: several
links mixing into one input.
