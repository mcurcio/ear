# AGENTS.md

This file provides guidance to coding agents working with code in this repository.

## What this is

EAR ("Einstein Audio Router") is a C++17 console app built on JUCE. It opens one audio output
device, builds a `juce::AudioProcessorGraph` of source nodes from a JSON config, routes their
channels to the device's output channels, and then serves a TCP port (7777) that is meant to
become a JSON-RPC control interface. The code is an early prototype (commits are "checkpoint"
snapshots from 2020).

## Build and run

Needs CMake 3.28 or newer (tested with 4.2.1), a C++17 compiler (tested with AppleClang 21 on
Apple Silicon), git and network access on the first configure. Nothing is installed:
`CMakeLists.txt` fetches JUCE (a release, pinned by commit) and Boost 1.92.0 (official CMake release
archive, pinned by SHA-256) with `FetchContent`, and the other libraries are vendored.

A build compiles only what EAR links. `ear_dependencies` in `src/CMakeLists.txt` is the one
link list for `ear` and `ear_tests`; its comment says why each entry is there.

- Boost: `program_options` is the only compiled library. `uuid`, `endian`, `asio_core` and
  Beast are header-only; Beast is linked for compilation only (`$<COMPILE_ONLY:...>`, hence
  CMake 3.28 with `FetchContent`'s `EXCLUDE_FROM_ALL`), and `CMakeLists.txt` does the same for
  `lexical_cast`'s link to the compiled `container`.
- JUCE: `juce_core`, `juce_events`, `juce_audio_basics`, `juce_audio_devices`,
  `juce_audio_processors_headless`, `juce_audio_formats` and `juce_dsp`. No GUI module, so no
  HarfBuzz, image or SVG libraries and no WebKit.

```bash
make debug      # configure out/ for Debug with -DBUILD_TESTS=OFF, then cmake --build --parallel
make release    # same with Release
make test       # configure Debug with -DBUILD_TESTS=ON, build ear_tests, run ctest --parallel
make JOBS=4 debug  # JOBS sets build and ctest parallelism (default: the CPU count)
make clean      # rm -rf out
./out/ear --help
```

- Build output goes to `out/` (gitignored); a post-build step copies the binary to `out/ear`.
- Fetched sources are cached in `.cache/deps/` (gitignored; `-DFETCHCONTENT_BASE_DIR=...`
  moves it): JUCE's shallow clone in `.cache/deps/juce-src/` (about 220 MB) and Boost's archive
  and source in `.cache/deps/boost-src/` (about 800 MB). Their build trees are
  `out/_deps/<name>-build`. `make clean` keeps the sources. `kept_fetchcontent`
  (`cmake/KeptFetchContent.cmake`) writes a marker file into each source tree recording the pin
  it was fetched from (repository and commit, or archive URL and SHA-256); when it matches the
  declared pin the next configure reuses the tree (`FETCHCONTENT_SOURCE_DIR_<NAME>`), otherwise
  it fetches again: JUCE re-clones (about 10 s), Boost re-verifies its archive and re-extracts
  (30-40 s). The build trusts a matching marker and does not re-check the tree: a tree edited
  or moved to another commit after its fetch is built as it is. Delete the tree to fetch it
  again (a missing tree, or one without its `CMakeLists.txt`, is fetched again even when
  `out/` survives).
- The build runs `cmake --build out --parallel $(JOBS)` with `MAKEFLAGS` cleared, so
  `JOBS`, not `make -jN`, sets its parallelism (a `-jN` is harmless but ignored). Configure
  runs only when `out/CMakeCache.txt` lacks the target's build type and `BUILD_TESTS`
  value; the build itself re-runs CMake when a `CMakeLists.txt` changes. JUCE compiles once
  per target. Measured here (8 cores, other jobs running): `make clean && make debug` 44 s
  (configure 20 s); `make test` after that 24 s; `make debug` after `make test` 2.4 s
  (reconfigures for `BUILD_TESTS=OFF`); a no-op `make debug` or `make test` 0.4-0.5 s.
- `ear` opens a real audio device: `--config` (default `ear.json`, relative to the working
  directory) and `--device` (index or UUID from the startup listing; overrides `"device"`).
  The other flags only feed disabled test functions in `src/main.cpp`. There is no lint config.

## Tests

`ear_tests` (Catch2 v2, one CTest test per test case) builds the graph with `run()`'s own
`ear::buildGraphFromConfig` (`include/ear/graph_config.hpp`) onto
`ear::test::VirtualAudioDevice` (`test/virtual_audio_device.hpp`), which never touches OS audio:
the test thread clocks it with `renderBlocks(n)`, which runs the callbacks on a separate audio
thread like a real device, and reads back every output channel.
The AirPlay cases write 16-bit little-endian stereo PCM (440 Hz left, 880 Hz right) into a
named pipe and check each tone arrives on its mapped channel sample for sample, unmapped
channels are exactly zero and an idle pipe is silent; others check white noise routing and pin a
device's UUID.
Verify audio here, never on real hardware. `make test` runs them all; to run one:

```bash
ctest --test-dir out -R "White noise" --output-on-failure  # by test name regex
./out/test/ear_tests_artefacts/Debug/ear_tests "[white]"   # by Catch tag
```

## CI

`.github/workflows/test.yml` runs on every pull request and every push to `master`, on
`macos-latest` (Apple Silicon) with CMake 4.2.1 (the image ships a newer one): `make test`, then
`make debug` (the `ear` binary, which `make test` does not build). On failure it uploads
`out/Testing` (ctest's logs) as the `ctest-output` artifact. It caches `.cache/deps/juce-src` and
`.cache/deps/boost-src`, keyed on `CMakeLists.txt` and `cmake/KeptFetchContent.cmake`; a tree
restored from an older key whose marker no longer matches its pin is fetched again. Pushes to `master` keep
the cache warm for pull requests, which can read it.

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
loading) and `src/server.hpp` (`run()`: device selection, then `buildGraphFromConfig` from
`include/ear/graph_config.hpp`, then the server loop). Each
executable includes the ear headers from one translation unit only (`src/main.cpp`,
`test/audio_graph_test.cpp`): `include/ear/server.hpp` defines a global
`jsonrpccxx::JsonRpc2Server rpcServer` and `pipe_source.hpp` defines a non-inline member
function, so including them from a second `.cpp` breaks the link.

How audio moves (this pull/push inversion is the non-obvious part):

1. `ear::AudioIoDevice` (`device.hpp`) wraps one `juce::AudioIODevice` and is itself the device
   callback. It fans each hardware callback (JUCE 9's `audioDeviceIOCallbackWithContext`) out
   to a list of registered `juce::AudioIODeviceCallback`s, in registration order. Devices are
   identified by a UUID made from `"<type name>:<device name>"`.
2. `ear::AudioGraphStandalone` (`graph.hpp`) is an `AudioGraph` that registers itself as one of
   those callbacks. On each device callback it runs `processBlock` on a scratch buffer: the
   device clock drives the graph. The device's start and stop events prepare and release the
   graph (as `juce::AudioProcessorPlayer` does); a JUCE 9 `AudioProcessorGraph` that was never
   prepared renders silence and never prepares its nodes.
3. `setDevice()` adds an `AudioIoDeviceOutputProcessor` node. It is a graph sink: its inputs are
   the device's output channels, and `processBlock` copies them into an internal buffer. It is
   also registered on the device, and its device callback copies that buffer to the hardware
   output. Connections from config therefore target this node, not JUCE's
   `AudioGraphIOProcessor` output node. It is registered before the graph, so the device
   receives the previous block's graph output (one block of latency).
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

Known defects the test works around: `AudioIoDevice::removeCallback` loops forever on a
started device; destroying a running `AirplayProcessor` calls `std::terminate` (`GraphSource`
does not forward `releaseResources`).

Sample rate 44100 and block size 512 are hard-coded in several places (`constants.hpp` exists
but most code repeats the literals).

## Vendored and fetched code

`vendor/json` (nlohmann), `vendor/jsonrpccxx`, `vendor/catch`, `vendor/jsonrpc` and
`vendor/libjsonrpc` were added with `git subtree`; leave them alone unless you are updating a
dependency. `ear` uses JUCE, nlohmann/json and jsonrpccxx (header-only); `ear_tests` adds Catch
2.12.2, whose x86-only debugger trap `test/main.cpp` overrides for Apple Silicon.
`vendor/jsonrpc` and `vendor/libjsonrpc` are unused.

JUCE and Boost are fetched, not vendored: the repository holds neither their code nor their
history. JUCE is cloned from `https://github.com/juce-framework/JUCE.git` at the commit of
the release tag that `juce_commit` in `CMakeLists.txt` names, shallow (depth 1). **The JUCE
tree is `.cache/deps/juce-src/`**: skills and docs cite JUCE files by that prefix (for example
`.cache/deps/juce-src/modules/juce_core/juce_core.h`), and the first configure creates it. Treat
it as read-only. To bump JUCE, set `juce_commit` in `CMakeLists.txt` (the one place the pin is
declared) to the commit of another release tag
(`git ls-remote --tags https://github.com/juce-framework/JUCE.git`) and update the release its
comments name, then configure: the marker no longer matches, so FetchContent clones the new
commit; then port and run the checks. The docs name the release only where it is a fact about
a check: `juce-architecture` says which release its claims were checked against, so re-check
them and update that line. To build
against a local JUCE checkout instead, pass `-DFETCHCONTENT_SOURCE_DIR_JUCE=<path>`. Bump Boost
by changing `boost_url` and `boost_sha256` together.

## JUCE references

JUCE here is the release pinned in `CMakeLists.txt` (`juce_commit`; the generated
`.cache/juce-api/MANIFEST` records its version); docs.juce.com may document another version. Two repo skills in
`.claude/skills/` cover it: `juce-api` (a symbol index and per-class pages generated from the
JUCE tree's headers, via `.claude/skills/juce-api/scripts/ensure.sh`) and `juce-architecture`
(which JUCE class fits each job in this repo, and the threading rules designs must respect).
