# Programs: slice 9 of the node framework

Status: design sketch, not built. Slice 9 lets a node run an outside program (first shairport-sync, for the `airplay`
type) whose audio joins the graph. It needs slice 3b's ring companions (`companions.md`) and slice 3c's loops, runtime
files and I/O executor (`node-primitives.md`). Only the shape is designed here; the program supervisor, the `airplay`
type's settings and shairport-sync's flags need their own design before code.

## 1. The owner's words that bind this slice

| # | The owner's words | Date | What it means here |
|---|---|---|---|
| R4 | "can i add my own sources and sinks?"; "the core will be \"built into ear\"" | 2026-10-29 | Types stay built in; a program is a resource a built-in type asks for |
| R5 | "i will want multiple shairport instances" | 2026-10-29 | Many nodes of one type, each with its own program |
| Q7 | Programs' first step: "Pipe into a feed first" | 2026-11-07 | Programs first join through a pipe into a paced source, not through PipeWire's socket |
| R9 | "Nodes should not have direct access to stdout or stderr. ..." | 2026-11-07 | A program's standard error goes into its node's log, never to ear's descriptors |

## 2. The design

### 2.1 The program resource

`cx.program({argv, stdout → paced source})`, asked in creation's `configure`: ear starts the program with its audio
on standard output. **Health is process-alive only** (design rule 1 of the system design: each health judgement its
owner's): ready when the process runs, lost when it exits (the node fails `lost`, and slice 8 recovers it). Silence on
the pipe between sessions is not a loss, so recovery never kills a healthy idle process. A program's standard error is
never an inherited descriptor 2: a framework thread reads it line by line into the node's log, and, through a line hook,
to the node's loop, so a type can turn "session started" into status (slice 5).

### 2.2 The paced source

A pipe has no clock: the program paces itself on its own host clock, and nothing tells it what played. So the pipe goes
into a **paced source**, a ring companion (`companions.md` 3.2's list, kind `paced-source`). *Since the owner's timing
model (2026-11-13) this is a source with a converter companion in tracked mode (`timing-model.md` 5.3), not a kind of
its own; what it does after a stall is Q-TM2's.*

| Kind | Joins | Engine name | Realtime side | Non-realtime side | Ring | Drops | Latency on its link |
|---|---|---|---|---|---|---|---|
| Paced source | an inside input bank | `<node>.<bank>` | a source with an adaptive resampler steered by its fill | a writer of raw PCM from a clockless source (the pipe) | the target fill and headroom | drops the oldest, counted | the target fill |

- `cx.pacedSource(bank, pcm{format, rate, channels}, latencyFrames)`: it converts the declared PCM (`pcm{s16le, 44100,
  2}` for shairport-sync [U: its exact flags]) to float and resamples it to the graph frequency with a fixed ratio plus
  an adaptive one, steered by a loop on the ring's fill (what rtp-source does from packet arrival).
- It declares its latency (the target fill) on its link, and the latency is passed to the program as a static latency
  offset where it takes one [U].
- A full ring never blocks the program beyond the pipe's own 64 KiB: the paced source drops the oldest audio and counts
  it.

### 2.3 Runtime files

`cx.runtimeFile(suffix, text)`: a file the node generates (the program's configuration: its name, ports, the pipe
backend), written beneath the config folder on the I/O executor and removed with its configuration; named
`<type>-<node>.<suffix>`.

### 2.4 Claims and discovery

The program's ports are claimed (`node-primitives.md` 6.8: shairport-sync's 5000 and 7000, `claim::TcpListen`), so two
`airplay` nodes, or an `airplay` node and another type, cannot take one port. mDNS: either an `advertise` resource kind
(`cx.advertise({service, port, name, txt})`, built from sendspin's `Advertiser` once a second type needs it) or the
program's own Avahi [U: whether shairport-sync can advertise without a D-Bus Avahi in the container].

## 3. What it costs

- **AirPlay loses multi-room alignment** with other AirPlay receivers: with no delay feedback through a pipe, the sender
  cannot align ear with its other receivers. Within ear the paced source holds the program's audio on the graph's clock.
- A process per node (R5), supervised by ear.

## 4. Later: serving PipeWire's socket

For programs that can only be PipeWire clients, ear could serve its engine's socket. Then the mark of a program's node
comes from the socket's peer credentials (`pipewire.sec.pid`), and ear writes the program's configuration to give its
nodes `<node>.<suffix>` names and to turn its autoconnect off. Such a program's nodes would be engine companions
(`companions.md` 3.2). Not designed.

## 5. Open questions

- [U] shairport-sync's raw PCM output flags, a static latency offset, and its mDNS without D-Bus in the container.
- The program supervisor's own design: start, stop (a signal and a deadline), restart on exit (slice 8), the
  environment and working directory a program gets, and resource limits.
- The `airplay` type's settings and which of them change in place (`reconfigure.md`).
