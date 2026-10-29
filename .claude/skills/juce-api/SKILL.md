---
name: juce-api
description: Look up the JUCE API this repo actually builds against (vendored JUCE 6.0.0 in juce/). Use before writing or reviewing any code that calls JUCE, before stating a JUCE class's signature, behaviour or threading rule, and whenever a JUCE compile error mentions a member that "does not exist". Gives a greppable symbol index and one Markdown page per class, generated from the vendored headers.
---

# JUCE API lookup (vendored JUCE 6.0.0)

This repo builds against the JUCE copy in `juce/` (version 6.0.0, 2020). docs.juce.com and
model memory describe JUCE 7 and 8, whose APIs differ (for example the device callback and
`AudioProcessorGraph` internals changed). Never take a JUCE signature from memory or the website:
look it up here.

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

`src/CMakeLists.txt` links `juce_core`, `juce_audio_devices`, `juce_audio_utils`,
`juce_audio_processors`, `juce_audio_formats` and `juce_dsp` (and so their dependencies, such as
`juce_events` and `juce_audio_basics`). A class from any other module needs a
`target_link_libraries` change first.

For which JUCE building blocks to use and the threading rules that constrain designs, use the
`juce-architecture` skill.

## Maintaining the generator

`scripts/generate.sh` runs Doxygen 1.9.4 in Docker (`scripts/Dockerfile`, Debian bookworm) with
`scripts/Doxyfile`, a small overlay on JUCE's own `juce/doxygen/Doxyfile` (same inputs, excludes
and predefines, XML output). `scripts/convert.py` turns the XML into the files above and writes
`MANIFEST` (JUCE version and the `juce/modules` git tree id that `ensure.sh` compares against).
Doxygen's warnings go to `.cache/juce-api/doxygen.log`.
