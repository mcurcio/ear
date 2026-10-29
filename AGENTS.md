# AGENTS.md

This file provides guidance to coding agents working with code in this repository.

## What this is

EAR ("Einstein Audio Router") is a C++17 console app built on JUCE. It opens one audio output
device, builds a `juce::AudioProcessorGraph` of source nodes from a JSON config, routes their
channels to the device's output channels, and then serves a TCP port (7777) that is meant to
become a JSON-RPC control interface. The code is an early prototype (commits are "checkpoint"
snapshots from 2020).

## Build and run

Needs CMake ≥ 3.12 and Boost ≥ 1.70 (`system`, `program_options`, `filesystem`) installed on
the system. JUCE and the other libraries are vendored in the tree.

```bash
make debug      # cmake -DCMAKE_BUILD_TYPE=Debug into out/, then make
make release    # same with Release
make clean      # rm -rf out
./out/ear --config ear.json --device 0
```

- Build output goes to `out/` (gitignored); a post-build step copies the binary to `out/ear`.
- `--device` takes a device index or the device UUID that the startup listing prints; it
  overrides `"device"` in the config. `--config` is resolved relative to the working directory
  (default `ear.json`). The other CLI flags (`--gain`, `--ramp`, `--channel`, `--connect`,
  `--quit`) only feed the disabled test functions in `src/main.cpp`.
- **There are no tests.** `make test` points at `out/test/main`, but the `BUILD_TESTS` block in
  `CMakeLists.txt` is commented out and there is no `test/` directory. Catch is vendored in
  `vendor/catch/` but not wired up. There is no lint config.

## Config file (`ear.json`)

Not committed; its shape comes from `run()` in `src/server.hpp`:

```json
{
  "device": "0",
  "airplay": { "buffer": 40 },
  "nodes": [
    { "type": "airplay", "pipe": "/tmp/airplay", "connect": [[0, 0], [1, 1]] },
    { "type": "white", "connect": [[0, 0]] }
  ]
}
```

Each `connect` pair is `[node output channel, device output channel]`. Node types are `white`
(mono white noise) and `airplay` (stereo, 16-bit little-endian PCM read from a named pipe, e.g.
shairport-sync's pipe backend). `airplay.buffer` is the ring size in 512-sample blocks.

## Architecture

All project code is header-only in `include/ear/`, plus `src/main.cpp` (CLI parsing, config
loading) and `src/server.hpp` (`run()`: device selection, graph build, server loop). There is a
single translation unit, so `include/ear/server.hpp` defines a global
`jsonrpccxx::JsonRpc2Server rpcServer` and `pipe_source.hpp` defines a non-inline member
function; including them from a second `.cpp` breaks the link.

How audio moves (this pull/push inversion is the non-obvious part):

1. `ear::AudioIoDevice` (`device.hpp`) wraps one `juce::AudioIODevice` and is itself the device
   callback. It fans each hardware callback out to a list of registered
   `juce::AudioIODeviceCallback`s, in registration order. Devices are identified by a UUID made
   from `"<type name>:<device name>"`.
2. `ear::AudioGraphStandalone` (`graph.hpp`) is an `AudioGraph` that registers itself as one of
   those callbacks. On each device callback it runs `processBlock` on a scratch buffer: the
   device clock drives the graph.
3. `setDevice()` adds an `AudioIoDeviceOutputProcessor` node. It is a graph sink: its inputs are
   the device's output channels, and `processBlock` copies them into an internal buffer. It is
   also registered on the device, and its device callback copies that buffer to the hardware
   output. Connections from config therefore target this node, not JUCE's
   `AudioGraphIOProcessor` output node.
4. Sources are `juce::AudioSource`s wrapped as graph nodes by `ear::GraphSource`
   (`processor.hpp`), which drives them through a `juce::AudioSourcePlayer`.
   `WhiteNoiseProcessor` and `AirplayProcessor` own their source and pass a raw pointer.
5. `PipeSource` (`pipe_source.hpp`) runs a reader thread that reopens the pipe file until it
   opens and polls it with 1 ms sleeps. `AirplaySource` converts samples into an interleaved
   float ring buffer guarded by a `juce::CriticalSection` and re-buffers half the ring whenever
   the reader catches up with the writer.

The network side is unfinished. `RpcServer` (async Beast acceptor on the main `io_context`)
accepts connections and drops them. `WebsocketServer` (a blocking thread per connection that
passes messages to `rpcServer.HandleRequest`) is never started, and no RPC methods are
registered.

Sample rate 44100 and block size 512 are hard-coded in several places (`constants.hpp` exists
but most code repeats the literals).

## Vendored code

`juce/`, `vendor/json` (nlohmann), `vendor/jsonrpccxx`, `vendor/catch`, `vendor/jsonrpc` and
`vendor/libjsonrpc` were added with `git subtree`; leave them alone unless you are updating a
dependency. Only JUCE, nlohmann/json and jsonrpccxx (header-only, via the include path) are used
by the build; `vendor/jsonrpc` and `vendor/libjsonrpc` are unused.

## JUCE references

JUCE here is 6.0.0, not the version on docs.juce.com. Two repo skills in `.claude/skills/` cover it:
`juce-api` (a symbol index and per-class pages generated from the vendored headers, via
`.claude/skills/juce-api/scripts/ensure.sh`) and `juce-architecture` (which JUCE class fits each job
in this repo, and the threading rules designs must respect).
