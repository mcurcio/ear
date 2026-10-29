---
name: juce-api
description: Look up the JUCE API this repo actually builds against (vendored JUCE 9.0.4 in juce/). Use before writing or reviewing any code that calls JUCE, before stating a JUCE class's signature, behaviour or threading rule, and whenever a JUCE compile error mentions a member that "does not exist". Gives a greppable symbol index and one Markdown page per class, generated from the vendored headers.
---

# JUCE API lookup (vendored JUCE 9.0.4)

This repo builds against the JUCE copy in `juce/` (version 9.0.4). Model memory mostly
describes JUCE 6 to 8, and docs.juce.com tracks the latest release, which need not be 9.0.4.
Memory of JUCE 6 or early 7 differs most (the device callback became
`audioDeviceIOCallbackWithContext` in 7.x, and `AudioProcessorGraph` lives in the
`juce_audio_processors_headless` module since 8.0.x); 9.0.x adds its own breaking changes,
listed per version at the top of `juce/BREAKING_CHANGES.md`. Never take a JUCE signature from
memory or the website: look it up here.

## 1. Get the reference

```bash
.claude/skills/juce-api/scripts/ensure.sh
```

Prints the reference directory (`.cache/juce-api/`, gitignored). It rebuilds it (about 10 seconds,
needs Docker) when it is missing or was built from a different `juce/modules` tree than the
current checkout. No Docker: skip to step 3 and read the headers.

## 2. Find things

| Question | Command |
|---|---|
| Does `X` exist, where is it, one-line summary | `grep -P '^juce::X\t' .cache/juce-api/symbols.tsv` |
| All members of a class | `grep -P '^juce::AudioProcessorGraph::' .cache/juce-api/symbols.tsv \| cut -f1,3` |
| Full docs for a class | Read `.cache/juce-api/classes/juce.AudioProcessorGraph.md` |
| Nested type | `classes/juce.AudioProcessorGraph.Node.md` (`::` becomes `.`) |
| Which class does a job | `grep -i 'fifo\|lock-free' .cache/juce-api/symbols.tsv \| cut -f1,6` |
| Macros (`DBG`, `jassert`, `JUCE_DECLARE_NON_COPYABLE`) | `grep -P '^DBG\t' .cache/juce-api/symbols.tsv` |
| What a module contains | Read `.cache/juce-api/modules.md` |

`symbols.tsv` columns: `name`, `kind` (class, struct, function, variable, enum, typedef, define),
`signature`, `location` (`juce/modules/...:line`), `module`, `brief`. Private members are left out.

A class page lists the class's module, where it's declared, its bases and derived classes, the
full description with code examples, and every public and protected member with its signature,
location and docs. It does not repeat inherited members: follow **Inherits:** to the base page.

## 3. Behaviour lives in the .cpp files

The reference covers headers only. For what a call actually does (locks, threads, allocation,
when a callback fires), open the `location` it gives and read the matching `.cpp` beside it in
`juce/modules/`. The headers and sources are the source of truth; the reference is a cache of
them. If they disagree, trust the headers and rerun `ensure.sh`.

## 4. Which modules are linked

`ear_dependencies` in `src/CMakeLists.txt` (shared by `ear` and `ear_tests`) links `juce_core`,
`juce_events`, `juce_audio_basics`, `juce_audio_devices`, `juce_audio_processors_headless`
(where `AudioProcessor` and `AudioProcessorGraph` live), `juce_audio_formats` and `juce_dsp`.
No GUI module is linked: `juce_audio_processors` (editors, plugin hosting UI) and
`juce_audio_utils` (`AudioProcessorPlayer`, `AudioDeviceSelectorComponent`) would pull in
`juce_gui_extra`, `juce_gui_basics` and `juce_graphics`. The `module` column of `symbols.tsv`
names a symbol's module. A class from any other module needs a `target_link_libraries` change
first, and its header included instead of `juce_audio_processors_headless.h`.

For which JUCE building blocks to use and the threading rules that constrain designs, use the
`juce-architecture` skill.

## Maintaining the generator

`scripts/generate.sh` runs Doxygen 1.9.4 in Docker (`scripts/Dockerfile`, Debian bookworm) with
`scripts/Doxyfile`, a small overlay on JUCE's own `juce/docs/doxygen/Doxyfile` (same inputs,
excludes and predefines, XML output). JUCE's file reads `juce/modules` in place (no preprocessing
step), so locations are exact `juce/modules` lines. It is written for Doxygen 1.17: 1.9.4 ignores
its newer tags, and the warnings about them in `doxygen.log` are expected. `scripts/convert.py` turns the XML into the files above and writes
`MANIFEST` (JUCE version and the `juce/modules` git tree id that `ensure.sh` compares against).
Doxygen's warnings go to `.cache/juce-api/doxygen.log`.
