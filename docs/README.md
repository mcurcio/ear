# EAR documentation

EAR ("Einstein Audio Router") is a C++23 program for Linux that runs PipeWire's audio engine inside
its own process and builds an audio graph from its graph file, `ear.conf`: it starts the nodes the
file names (white noise, WAV recorders, sound cards, RTP streams, Music Assistant over Sendspin),
links their ports as the file's routes say, prints a JSON record for every change, and stops cleanly
on SIGINT or SIGTERM. Its control port (7777) serves a graph editor and a JSON-RPC interface that
edit the running graph and write the edits back into `ear.conf`; hand edits to `ear.conf` are
applied as soon as they are saved. It ships as a container image.

These pages describe ear as it is built. Building, testing, CI and releases are in
[`AGENTS.md`](../AGENTS.md); designs for parts not built yet are in [`design/`](design/).

## Reading order

| If you want to | Read |
|---|---|
| Understand what ear is and how its parts fit | [architecture.md](architecture.md) |
| Write or edit `ear.conf` | [graph-file.md](graph-file.md), then [node-types.md](node-types.md) |
| Configure a node: settings, ports, behaviour, failures | [node-types.md](node-types.md) |
| Drive ear from a script or write a client | [control-port.md](control-port.md) |
| Use or change the graph editor in the browser | [editor.md](editor.md) |
| Read or parse ear's log | [output-records.md](output-records.md) |
| Add a node type | [writing-a-node-type.md](writing-a-node-type.md), after [architecture.md](architecture.md#the-node-lifecycle) |
| Understand how audio time works: one tick, one rate, clocks at the graph's edges | [architecture.md](architecture.md#the-timing-model), then [engine.md](engine.md#the-clock) |
| Change the engine side, or understand the clock, latency and PipeWire's behaviour | [engine.md](engine.md) |
| Know what the tests prove and the rules they follow | [testing.md](testing.md) |

Working guides for specific jobs are the repository's skills in `.claude/skills/`: `pipewire-engine`
(changing `src/pipewire/`), `control-port` (talking to port 7777), `sendspin` (pairing and operating
a `sendspin` node) and `ear-on-talos` (running the image on Talos with the MOTU 24Ao, FPP's RTP
stream and a `sendspin` node in a pod). Research notes are in [`research/`](research/).

## A first run

```conf
# config/ear.conf
graph_frequency 48000;
block_size 256;
node white noise;
node file rec { path "take1.wav"; }
route noise:noise.mono rec:record.left;
```

```bash
./out/ear                                   # reads config/ear.conf, writes recordings/take1.wav
node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc graph.get
```

Open `http://127.0.0.1:7777/` for the editor (with `--web-root web/dist` after `make web` in a
local build). Stop ear with Ctrl-C: `recordings/take1.wav` holds white noise on its left channel and
exact silence on its right.

## Words

| Word | Meaning |
|---|---|
| graph file | `ear.conf`, the one file that says what nodes and routes are wanted |
| graph file folder | `--config-dir`: holds `ear.conf` and `sendspin` key files |
| graph frequency, block size | The engine's sample rate and frames per cycle, fixed at boot |
| node | One instance of a node type, named in `ear.conf` |
| port block | A node's one PipeWire filter, made once for the node's life, with one mono port per declared port |
| bank | A named group of a node's ports, all one direction; a port is `<bank>.<member>` |
| inside bank | A bank no route can name, reached only by the node's own code and engine companions |
| route | A link from one node's output port to another's input port |
| configuration | What a node's settings make behind its block: the type's instance, its resources and its code. Restarted, it is torn down and made again |
| engine companion | A PipeWire node a configuration asks for (a sound card's ALSA node, an RTP receiver), joined to an inside bank |
| sink | A lock-free ring and a thread that take a bank's audio off the realtime thread (a `file` node's writer) |
| internal clock | The dummy driver that always drives the graph, tracking `CLOCK_MONOTONIC_RAW` |
| desired graph | The wanted nodes and routes, and the one judge of every change |
| keeper | The graph file keeper: owns the working copy of `ear.conf`, watches and writes it |
| working copy, candidate | The keeper's text of `ear.conf`; one edit applied to a fresh load of it, judged before it is kept |
| phase, state, wanted | A node's exact lifecycle state; its colour; what the graph wants of it |
| note | A line a node's insides say (`node kitchen says paired ...`), never a state |
| record | One JSON line on standard output |
