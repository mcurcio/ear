# Batch 2: run image, control port, graph UI, sound card

Status: scope answered by the owner on 2026-10-30; the module design and slice pages are added
below by the batch's code architect. The system design is `docs/design/pipewire-graph.md`.

## The owner's answers (chat, 2026-10-30)

The question page: https://claude.ai/artifact/DraHdKRyZDB4WwBWxDTn4k

| Question | His words | What it means here |
|---|---|---|
| Scope | "Ok" (to the recommended scope) | Run image, control port, web UI, sound card node. Sendspin (Music Assistant) is the next batch. |
| Web UI | "Graph view is the goal. Dynamically wiring up nodes and edges in a UI is the goal. I want to see all of the graph information in real time, like the audio levels per-channel / per edge" | A drawn graph editor: nodes as boxes, routes as wires added and removed in the UI, with live status and live audio levels per channel and per route. |
| Hardware | "Motu 24ao. Talos 1.13.x I think maybe 1.14.x" | A MOTU 24Ao (AVB series, USB class compliant). Talos 1.13 or later includes the USB audio driver (added in 1.12). |
| Threading | "The control port shouldn't run in the PipeWire context, because it will not be real time" | The control port runs on its own thread, never on PipeWire's threads. |

Not answered: the CPU of the Talos nodes. The image is built for amd64 until he says otherwise.

Note on the threading answer: in EAR today audio runs on PipeWire's realtime data thread, and
graph edits and link events run on PipeWire's main loop, which is not realtime. The control port
still gets its own thread, so a slow or stuck client never delays graph edits or link events.

## The ideas, as researched (2026-10-30)

- "build a docker container in GHA": a run image without compilers, published to ghcr.io on
  every merge to master and tagged by commit; versioned images only from an automated release
  flow.
- "add MOTU drivers to image": no MOTU driver exists or is needed. MOTU's USB interfaces use the
  kernel's USB audio class driver (`snd-usb-audio`), which is in the Talos kernel, not the image.
  The image already carries PipeWire's ALSA plugin. The work is a sound card node type, with the
  card driving the clock while present.
- "support home assistant network audio as source + sink": Music Assistant's Sendspin protocol
  (deferred to the next batch).
- "add control socket": port 7777, JSON-RPC 2.0 over WebSocket (Snapcast's pattern).
- "add web UI": served by EAR on the same port.

## Design choices (current choices, revisable)

- The sound card drives the clock while present; the dummy driver stays as a fallback so the
  graph keeps running without it (R2).
- Removing a node removes its routes, and status says so.
- New settings for a node restart its insides; its ports and routes stay. Changing ports is
  refused.
- Live edits are not saved; `ear.json` loads at boot.
- No login on port 7777 yet; it refuses browser pages from other origins. Keep it in the cluster.
- A sound card is named in `ear.json` by card name (for example `hw:CARD=...`), never by index.
