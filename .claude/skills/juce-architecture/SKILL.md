---
name: juce-architecture
description: Which JUCE 6.0 building blocks fit this repo's audio-routing jobs, and the JUCE threading and real-time rules a design must respect. Use when designing or reviewing architecture in this repo (device handling, the routing graph, sources, cross-thread hand-off, the control server), when judging whether a bespoke mechanism duplicates something JUCE already provides, or when briefing an architect or implementer on JUCE work.
---

# JUCE building blocks and rules for EAR

Prior art first: before designing a mechanism, find the JUCE class that already does the job
(table below), then look up its exact API with the `juce-api` skill. Every claim here was checked
against the vendored JUCE 6.0.0 source; the `file:line` refs are under `juce/modules/`. Newer JUCE
(7, 8) changed several of these, so do not mix in docs.juce.com or remembered behaviour.

## Which JUCE class does the job

| Job | JUCE 6.0 building block | Module (linked?) | This repo today |
|---|---|---|---|
| Open, choose and configure an audio device (type, name, rate, block size) | `AudioDeviceManager`, `AudioDeviceManager::AudioDeviceSetup` | juce_audio_devices (yes) | `ear::AudioIoDevice` wraps `AudioIODevice` by hand |
| Play a processor or graph on a device | `AudioProcessorPlayer` (is an `AudioIODeviceCallback`) | juce_audio_utils (yes) | `AudioGraphStandalone` + `AudioIoDeviceOutputProcessor` |
| Route channels between nodes | `AudioProcessorGraph`, its `AudioGraphIOProcessor` (`audioOutputNode` = the device's outputs) | juce_audio_processors (yes) | `ear::AudioGraph` (thin wrapper, fine) |
| Pull-style sources | `AudioSource`, `AudioSourcePlayer`, `MixerAudioSource`, `ChannelRemappingAudioSource`, `BufferingAudioSource` | juce_audio_basics / juce_audio_devices (yes) | `GraphSource` wraps sources as nodes |
| Hand audio from a reader thread to the audio thread without a lock | `AbstractFifo` (single reader, single writer) over an `AudioBuffer<float>` | juce_core (yes) | `AirplaySource`: ring buffer + `CriticalSection` |
| Wait for something to happen instead of polling | `WaitableEvent`, `Thread` (`notify`, `wait`, `signalThreadShouldExit`) | juce_core (yes) | `PipeSource`: 1 ms sleep loop, 5 s retry |
| Smooth gain and parameter changes | `SmoothedValue`, `dsp::Gain` | juce_audio_basics / juce_dsp (yes) | `GainProcessor` uses `dsp::Gain` |
| Fixed DSP chains | `dsp::ProcessorChain`, `dsp::ProcessSpec` | juce_dsp (yes) | none |
| Run work on the message thread | `MessageManager::callAsync`, `runDispatchLoop` | juce_events (yes, via deps) | no dispatch loop runs |

Using one of these needs no justification. A bespoke replacement must say why the JUCE class
does not fit.

## Rules a design must respect

**1. `AudioProcessorGraph` only rebuilds on the message thread.** Adding or removing a node or
connection calls `topologyChanged()`. That rebuilds the render sequence right away only when the
caller is the message thread; otherwise it posts an async update
(`juce_audio_processors/processors/juce_AudioProcessorGraph.cpp:22-28`, `:899-905`).
`prepareToPlay` follows the same path (`:1257-1267`). The posted update only runs if something
runs the message loop.
- In a console app, the message thread is whichever thread first calls
  `MessageManager::getInstance()` (`juce_events/messages/juce_MessageManager.cpp:26-27`).
- This repo runs no dispatch loop: `main` blocks in Boost.Asio's `io.run()`.
- So graph edits made from any other thread (an RPC handler, the audio thread) never take
  effect. A control-plane design must deliver every graph edit to one thread that is the message
  thread: either that thread runs `runDispatchLoop` and edits arrive through `callAsync`, or the
  edits run there synchronously.

**2. The device callback contract** (`juce_audio_devices/audio_io/juce_AudioIODevice.h:51-83`):
fill every output channel, writing zeros for silence, because the buffer starts undefined.
Channels that weren't opened are null pointers. `numSamples` can change between callbacks. Code
that assumes 512 samples or 44100 Hz breaks this.

**3. `AudioProcessorPlayer` sets the channel counts, sample rate and block size from the
device.** When the device starts, it reads the device's active channels, rate and block size,
then calls `setPlayConfigDetails` and `prepareToPlay` on its processor
(`juce_audio_utils/players/juce_AudioProcessorPlayer.cpp:33-60`, `:179-204`). A graph played this
way gets its configuration from the device. Nothing needs hard-coding.

**4. `AudioDeviceManager` mixes multiple callbacks.** The first callback writes the output; each
other callback renders into a temporary buffer that is added on top
(`juce_audio_devices/audio_io/juce_AudioDeviceManager.cpp:806-841`). It holds
`audioCallbackLock` while doing this, and `addAudioCallback` takes the same lock (`:772-784`).
Registration order does not change what is heard. In this repo's `AudioIoDevice` it does: each
callback overwrites the output.

**5. Keep the audio thread free of contended locks, allocation and I/O.** JUCE 6 takes a few
short internal locks on the audio thread (graph, player and device manager callback locks).
Don't add locks that a slow thread can hold. Move data between threads with `AbstractFifo`, and
send control changes as atomics or `SmoothedValue` targets.

## Applying this to a design

1. Name the JUCE building block for each job and cite its `juce/modules/...` location.
2. State which thread each operation runs on: audio, message, reader or network. Rule 1 makes
   this a design question, not an implementation detail.
3. Check every claim about JUCE behaviour in the `.cpp` before a design rests on it. Use the
   `juce-api` skill to find it.
4. Map each bespoke `ear::` class to the JUCE class it would be replaced by. Keep it only if
   it adds behaviour JUCE lacks; say what behaviour that is.
