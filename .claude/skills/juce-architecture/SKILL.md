---
name: juce-architecture
description: Which JUCE 9 building blocks fit this repo's audio-routing jobs, and the JUCE threading and real-time rules a design must respect. Use when designing or reviewing architecture in this repo (device handling, the routing graph, sources, cross-thread hand-off, the control server), when judging whether a bespoke mechanism duplicates something JUCE already provides, or when briefing an architect or implementer on JUCE work.
---

# JUCE building blocks and rules for EAR

Prior art first: before designing a mechanism, find the JUCE class that already does the job
(table below), then look up its API with the `juce-api` skill. Every claim here was checked
against the JUCE 9.0.4 source; JUCE `file:line` refs are under `.cache/deps/juce-src/modules/`
(the JUCE tree CMake fetches; see AGENTS.md). Do not
mix in older JUCE or remembered behaviour.

## Which JUCE class does the job

| Job | JUCE 9 building block | Module (linked?) | This repo today |
|---|---|---|---|
| Open and configure an audio device (type, name, rate, block size) | `AudioDeviceManager`, `AudioDeviceManager::AudioDeviceSetup` | juce_audio_devices (yes) | `ear::AudioIoDevice` wraps `AudioIODevice` by hand |
| Play a processor or graph on a device | `AudioProcessorPlayer` (is an `AudioIODeviceCallback`) | juce_audio_utils (no: linking it adds the GUI modules) | `AudioGraphStandalone` + `AudioIoDeviceOutputProcessor` |
| Route channels between nodes | `AudioProcessorGraph`, its `AudioGraphIOProcessor` (`audioOutputNode` = the device's outputs) | juce_audio_processors_headless (yes) | `ear::AudioGraph` (thin wrapper, fine) |
| Pull-style sources | `AudioSource`, `AudioSourcePlayer`, `MixerAudioSource`, `ChannelRemappingAudioSource`, `BufferingAudioSource` | juce_audio_basics / juce_audio_devices (yes) | `GraphSource` wraps sources as nodes |
| Hand audio from a reader thread to the audio thread without a lock | `AbstractFifo` (single reader, single writer) over an `AudioBuffer<float>` | juce_core (yes) | `AirplaySource`: ring buffer + `CriticalSection` |
| Wait for something to happen instead of polling | `WaitableEvent`, `Thread` (`notify`, `wait`, `signalThreadShouldExit`) | juce_core (yes) | `PipeSource`: 1 ms sleep loop, 5 s retry |
| Smooth gain and parameter changes | `SmoothedValue`, `dsp::Gain` | juce_audio_basics / juce_dsp (yes) | `GainProcessor` uses `dsp::Gain` |
| Fixed DSP chains | `dsp::ProcessorChain`, `dsp::ProcessSpec` | juce_dsp (yes) | none |
| Run work on the message thread | `MessageManager::callAsync`, `callSync`, `runDispatchLoop` | juce_events (yes) | no dispatch loop runs |

Using one of these needs no justification; a bespoke replacement must say why it does not fit.

## Rules a design must respect

**1. `AudioProcessorGraph` rebuilds edits right away only on the message thread.** Each edit
(`addNode`, `addConnection`...) takes an `UpdateKind`, default `sync`
(`juce_audio_processors_headless/processors/juce_AudioProcessorGraph.h:239-246`). `sync` rebuilds
now only on the message thread, otherwise posts an async update
(`RebuildKind::syncIfMainThread`); `async` always posts; `none` waits for `rebuild()`
(`juce_AudioProcessorGraph.cpp:1915-1975`). Only `prepareToPlay` and `releaseResources` rebuild
immediately on any thread (`:1843-1861`). Posted updates need a message loop.
- The message thread is whichever thread first calls `MessageManager::getInstance()`
  (`juce_events/messages/juce_MessageManager.cpp:38-39`, `:72-81`); the graph's rebuild check
  calls it, so the first thread to edit a graph claims it: here, `main`.
- This repo runs no dispatch loop: `main` builds the graph, then blocks in Boost.Asio's `io.run()`
  (`src/server.hpp:69-123`), so edits from any other thread (such as a socket thread) never
  take effect.
- A control-plane design must deliver every graph edit to the message thread: run
  `runDispatchLoop` there and post edits with `callAsync`, or run them there synchronously.

**2. The device callback contract** (`juce_audio_devices/audio_io/juce_AudioIODevice.h:72-117`):
`audioDeviceIOCallbackWithContext` has an empty default body, so always write `override`. Fill
every output channel, writing zeros for silence, because the buffer starts undefined.
Channels that weren't opened are null pointers. `numSamples` can change between callbacks: don't
assume 512 samples or 44100 Hz.

**3. `AudioProcessorPlayer` configures its processor from the device.** On device start it reads
the active channel counts, rate and block size
(`juce_audio_utils/players/juce_AudioProcessorPlayer.cpp:344-373`), uses the device's channel
layout if the processor supports it, else its default (`:118-139`), then calls
`setPlayConfigDetails` and `prepareToPlay` (`:152-180`).

**4. `AudioDeviceManager` mixes multiple callbacks.** The first callback writes the output; each
other callback renders into a temporary buffer that is added on top
(`juce_audio_devices/audio_io/juce_AudioDeviceManager.cpp:1089-1131`). It holds
`audioCallbackLock` while doing this, and `addAudioCallback` takes the same lock (`:1055-1069`).
Registration order does not change what is heard; in this repo's `AudioIoDevice` every callback
shares one output buffer and the last writer wins.

**5. Keep the audio thread free of contended locks, allocation and I/O.** JUCE's audio-thread
locks are short (device manager, player, processor callback; the graph only try-locks,
`juce_AudioProcessorGraph.cpp:1684-1694`). Don't add locks that a slow thread can hold. Move data
between threads with `AbstractFifo`, and send control changes as atomics or `SmoothedValue`
targets.

## Applying this to a design

1. Name the JUCE building block for each job and cite its `.cache/deps/juce-src/modules/...` location.
2. State which thread each operation runs on: audio, message, reader or network. Rule 1 makes
   this a design question, not an implementation detail.
3. Check every claim about JUCE behaviour in the `.cpp` before a design rests on it. Use the
   `juce-api` skill to find it.
4. Map each bespoke `ear::` class to the JUCE class it would be replaced by. Keep it only if
   it adds behaviour JUCE lacks; say what behaviour that is.
