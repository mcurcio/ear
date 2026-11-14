# AGENTS.md

This file guides coding agents working in this repository. It says how to build, test and change
ear, and where everything else is written down; it does not describe ear itself, which the library
docs in `docs/` do.

## What this is

EAR ("Einstein Audio Router") is a C++23 program for Linux that runs PipeWire's audio engine inside
its own process and builds an audio graph from its graph file, `ear.conf` (written in the knobly
language, mcurcio/knobly). It starts the nodes the file names (`white`, `file`, `card`, `rtp`,
`sendspin`), links their ports as the file's routes say, prints a JSON record for every change, and
keeps the file live: edits made through its control port (7777: a graph editor and JSON-RPC) are
applied and written back into `ear.conf`, and hand edits are applied and shown in the editor. It
ships as a container image published from GitHub Actions.

## Where things are documented

| What | Where |
|---|---|
| How ear works, as built: architecture, the graph file, node types, writing a node type, the control port, standard output's records, the engine, the editor, testing | `docs/` ([`docs/README.md`](docs/README.md) is the index and reading order) |
| The timing model: one tick at one graph rate, clocks and rate conversion only in edge nodes | [`docs/architecture.md#the-timing-model`](docs/architecture.md#the-timing-model) (as built, and where the code still departs); its unbuilt part is [`docs/design/timing-model.md`](docs/design/timing-model.md) |
| Designs for work not built yet (slices 3c, 3d, 5, 6, 8, 9, cards and the clock, the root classes behind slice 3's fix clusters) | [`docs/design/`](docs/design/README.md) |
| Working guides for specific jobs | The skills in `.claude/skills/`: `pipewire-engine` (changing `src/pipewire/` or `engine.conf`, engine traps), `control-port` (talking to port 7777), `sendspin` (pairing and operating a `sendspin` node, the protocol library), `ear-on-talos` (the image on Talos with the MOTU 24Ao, FPP's RTP stream, `sendspin` in a pod) |
| Research notes | `docs/research/` (`motu-24ao-drivers/`: MOTU's macOS driver against `snd-usb-motu`, and the 24Ao's USB descriptors) |

Rules for the docs:

- **Every fact has one home.** The library docs describe ear as built; a change to the code
  changes the page that describes it in the same commit. Point at a page (by heading anchor, as
  `docs/engine.md#drive-and-fade`) rather than repeat it.
- **Design docs are for unbuilt work only.** When a slice is built, its design page is deleted in
  the same change and what lasts of it (how it works, why) moves into the library docs. Code
  comments never cite a design page's section numbers or decision ids for built work; they say the
  reason plainly or point at a library doc.
- **The wire changes in one commit with its description**: `docs/control-port.md`, the fixtures in
  `test/protocol/`, `src/control/` and `web/src/protocol.ts` together
  ([`docs/control-port.md#changing-the-wire`](docs/control-port.md#changing-the-wire)).

## Build, run and test

EAR builds and runs on Linux only (it links PipeWire); configure stops on any other platform. Build
and test in the project's image, `docker/Dockerfile` (Debian 13 "trixie": PipeWire 1.4.2, GCC 14,
CMake 3.31), through `scripts/dev.sh`, which builds the image when the Dockerfile changed, mounts
the checkout at the same path and runs the command as you (its header says how). On a Mac this is
the only way to build; it needs Docker.

```bash
scripts/dev.sh make test          # configure Debug with -DBUILD_TESTS=ON, build everything, run ctest
scripts/dev.sh make debug         # configure Debug with -DBUILD_TESTS=OFF and build ear
scripts/dev.sh make release       # same with Release
scripts/dev.sh make JOBS=4 test   # JOBS sets build and ctest parallelism (default: the CPU count)
scripts/dev.sh make clean         # rm -rf out
scripts/dev.sh make test CMAKE_ARGS=-DFETCHCONTENT_SOURCE_DIR_BOOST=<path>/.cache/deps/boost-src
scripts/dev.sh make web           # npm ci, then build the graph editor into web/dist
scripts/dev.sh make web-test      # npm ci, typecheck, vitest, build
scripts/dev.sh make format        # clang-format -i over the tracked C++ under src/ and test/
scripts/dev.sh make format-check  # the same, failing on any file it would change (CI's format job)
scripts/dev.sh bash               # a shell in the image

scripts/dev.sh ctest --test-dir out -R "lands on left" --output-on-failure   # one test, by name regex
scripts/dev.sh out/src/nodes/white/ear_node_white_tests "[white]"          # one suite, by Catch tag

./out/ear                                                  # ./config/ear.conf; file nodes write into ./recordings
./out/ear --config-dir ~/ear-config --web-root web/dist    # another graph file folder; the editor built by make web
node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc graph.get   # one control-port call from a script
```

`ear --help` lists the flags; [`docs/architecture.md#boot-and-stop`](docs/architecture.md#boot-and-stop)
says what each does and how ear boots and stops. `ear` reads `engine.conf` from beside its own
binary.

Toolchain notes:

- CMake 3.28 or newer (the `$<COMPILE_ONLY:...>` link and `FetchContent`'s `EXCLUDE_FROM_ALL`),
  pkg-config, `libpipewire-0.3`, `libspa-0.2` and their run-time modules, `sndfile`, and `git` (it
  clones the `sendspin` node's dependencies); the Dockerfile's comments say why each package is
  there. Configure names every pkg-config package it did not find.
- Build output goes to `out/` (gitignored). A post-build step copies the binary to `out/ear`, and
  configure copies `src/pipewire/engine.conf` beside it to `out/engine.conf`.
- `JOBS`, not `make -jN`, sets the build's parallelism, and configure runs only when
  `out/CMakeCache.txt` lacks the target's build type and `BUILD_TESTS` value or `CMAKE_ARGS` is
  given (the `Makefile` says why).
- Boost 1.92.0 (pinned by SHA-256): `program_options` is its only compiled library, and Beast and
  Asio (header-only) serve the control port; configure fails naming Beast if it is not there. Its
  source tree is kept in `.cache/deps/boost-src/` (about 800 MB, gitignored), which `make clean`
  keeps; `cmake/KeptFetchContent.cmake` says when a kept tree is reused and when it is fetched
  again. Each checkout keeps its own: a worktree with no `CMAKE_ARGS` fetches and keeps its own
  copy, so point `CMAKE_ARGS` at a tree another checkout already keeps (as above); the main
  checkout may have none. Bump Boost by changing `boost_url` and `boost_sha256` in
  `CMakeLists.txt` together.
- knobly comes from the dev image's `/opt/knobly`: its C++ release tree, which the Dockerfile's
  throwaway `knobly` stage builds from a pinned commit with knobly's own release script (python3,
  uv and a clang-format download, at image build only). ear adds it as a subproject in compiled
  mode (`src/config/knobly_implementation.cpp`), and only `ear_config` links it. A Linux build
  outside the image points the CMake cache variable `EAR_KNOBLY_DIR` at such a tree
  (`CMAKE_ARGS=-DEAR_KNOBLY_DIR=<path>`); configure fails naming it when
  `include/knobly/knobly.hpp` is not there. Bump knobly by changing `KNOBLY_COMMIT` in
  `docker/Dockerfile` (the pin's one home); `scripts/dev.sh` rebuilds the image.
- The `sendspin` node's fetched dependencies are in `cmake/SendspinDeps.cmake`, each at a full
  commit or by SHA-256: sendspin-cpp (with `cmake/sendspin-listener.patch`,
  `sendspin-starved-floor.patch` and `sendspin-late-chunk.patch`; a failed apply, or a tree without
  one, stops configure), ArduinoJson, IXWebSocket (likewise,
  `cmake/ixwebsocket-limits.patch`), micro-flac, noise-c and mjansson/mdns (one header). Each is
  kept in `.cache/deps/` as Boost's is, and `ear_node_sendspin` is the only library that sees
  them. Configure copies their licences to `out/licenses/`. With `BUILD_TESTS=ON` configure stops
  naming `EAR_SENDSPIN_SERVER_PYTHON` when the image's `/opt/sendspin-server/bin/python` (the real
  Sendspin server the tests run against, `test/sendspin/requirements.txt`) is missing.
- ccache is the image's compiler launcher; `scripts/dev.sh` points `CCACHE_DIR` at the main
  checkout's `.cache/ccache`, shared by every worktree.
- The graph editor (`web/`) builds with the image's Node.js 24 (`web/.nvmrc`);
  `web/package-lock.json` pins its packages and `npm ci` installs exactly those. npm's cache is the
  main checkout's `.cache/npm`. Developing it with hot reload: [`docs/editor.md`](docs/editor.md#developing-it).

## Repository layout

| Path | Holds |
|---|---|
| `src/<package>/` | One C++ package each (`core`, `runtime`, `folder`, `pipewire`, `output`, `control`, `config`, `server`, `all_node_types`), with its own `CMakeLists.txt`, public headers in `include/ear/<package>/` and tests in `tests/` |
| `src/nodes/<type>/` | One node type each, its own library; a folder here registers its type and nothing else names it ([`docs/writing-a-node-type.md`](docs/writing-a-node-type.md) is how to add or remove one) |
| `src/main.cpp` | The composition root: wiring, boot and stop order, glue |
| `web/` | The graph editor; shares no code with the C++, only the wire format and `test/protocol/` |
| `test/` | `protocol/` (the wire's fixtures), `system/` (end-to-end tests through `out/ear`), `nodes/` (what holds for every type), `support/` |
| `cmake/`, `scripts/`, `docker/` | Build helpers and pins, dev and CI scripts, the images |
| `vendor/` | Vendored code (below) |

The packages, what each owns and must never do, and the dependency rules are in
[`docs/architecture.md#packages`](docs/architecture.md#packages); keep them: dependencies point
one way, an interface lives with the package that consumes it, no node type links another, nor
`ear_runtime` or `ear_pipewire` (configure refuses each), and no Boost, knobly or sendspin-cpp type
appears in a public header.

## Conventions

- **Style.** `.clang-format` (tabs for indent, 120 columns) over the tracked C++ under `src/` and
  `test/`; run `make format` before committing, as CI's `format` job fails on any file it would
  change. There is no lint config.
- **Tests.** Tests are event-gated: they wait for the
  event they need, and a deadline only fails a test. Nothing is skipped: a test that cannot run
  fails naming why. Verify audio in the tests, never on real hardware. A timing test that fails on
  a loaded host is rerun alone before the code is suspected. The rules and what each suite proves:
  [`docs/testing.md`](docs/testing.md).
- **Comments** say why, and cite the library doc that holds the rule (by heading anchor), never a
  review record, a step or a decision id.

### Commits, CI and releases

- **Conventional Commits.** Releases are automatic: merging a pull request to `master` is the
  release. A squash-merge counts only its title, so the title must be a Conventional Commit.
  After the checks pass on that push, `test.yml`'s `release` job runs semantic-release
  (`.releaserc.json`, its tooling pinned in the root `package.json` and `package-lock.json`,
  under `web/.nvmrc`'s Node): the commits since the last `v*` tag decide the bump (`fix` a patch,
  `feat` a minor, `build(deps)` a patch, `!` or `BREAKING CHANGE:` a major, other types none; the
  notes list Features, Bug Fixes, Performance, Reverts and Dependencies and hide every other
  type). It appends the notes to `CHANGELOG.md`, commits that as `chore(release): <version> [skip
  ci]`, tags it `v<version>` and makes the GitHub Release. The version is the tag; no file holds
  it. Never tag or version by hand, with one exception: if the release job fails after it pushed
  the `CHANGELOG.md` commit and tag (making the GitHub Release failed), a rerun finds `master`
  ahead and releases nothing, and nothing reruns from the new head; make that GitHub Release by
  hand from its `CHANGELOG.md` section (`gh release create v<version> --verify-tag`). That
  version's image is then not published.
- **CI** (`.github/workflows/test.yml`) runs on every pull request and every push to `master`, four
  `ubuntu-24.04` jobs in parallel: `test` (builds the dev image with buildx, layers in GitHub's
  cache, restores the fetched trees and ccache, runs `scripts/dev.sh make test` with
  `EAR_DEV_IMAGE=ear-dev`, and uploads ctest's logs as `ctest-output` on failure), `web` (`make
  web-test`), `format` (`make format-check`) and `image` (checks `scripts/version-tags.sh`, builds
  the run image and runs `scripts/image-smoke.sh` on it). On a push to `master`, once all four
  pass, `release` and then `publish` run: `publish` pushes the run image to
  `ghcr.io/mcurcio/ear`, built from the release's tag (else the merged commit) and tagged
  `master`, `sha-<merged commit>` and one version per `v*` tag on it (`scripts/version-tags.sh`).
  A newer push cancels a pull request's run, never a `master` run. The workflow's comments give
  the reasons for each step and cache.
- **The images.** `docker/Dockerfile` has three targets, `dev` (the build and test image),
  `build` (`make release` and the editor's build) and `run` (what ships:
  [`docs/architecture.md#the-container-image`](docs/architecture.md#the-container-image)), and a
  throwaway stage, `knobly`. To check the run image locally:

  ```bash
  docker build --target run --tag ear:local --file docker/Dockerfile .
  scripts/image-smoke.sh ear:local   # what CI checks: user, no compiler, files, ear.conf, graph.get, graphFile.get
  ```

### Vendored and fetched code

`vendor/json` (nlohmann) and `vendor/catch` (Catch 2.12.2) are used; `vendor/jsonrpccxx`,
`vendor/jsonrpc` and `vendor/libjsonrpc` are not built (the control port has its own JSON-RPC code
in `ear_control`; removing them is deferred). All were added with `git subtree`; leave them alone
unless you are updating a dependency. Update one with `git subtree pull --prefix vendor/<x> <repo>
<ref> --squash -m "build(deps): upgrade <X> to <a.b.c>"`: with `--squash` the upstream commits
never enter ear's history, so the release notes get only the merge commit's message (one
Dependencies line and a patch release); the squash commit's own message is not a Conventional
Commit, so it is ignored. Bumps of the fetched dependencies (Boost's `boost_url` and
`boost_sha256`, `KNOBLY_COMMIT`, those in `cmake/SendspinDeps.cmake`) use the same message. The
subtrees were first added without `--squash`, so their upstream histories are in ear's history;
the baseline tag `v0.0.0` on cb6d71f64c (the last `git subtree add`, 2020-05-27) ends them, since
semantic-release reads only the commits after the last `v*` tag. PipeWire, alsa-lib and libsndfile
come from the image's Debian packages. Bumping PipeWire: the `pipewire-engine` skill.
