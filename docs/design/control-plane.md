# The control plane: what slice 5 left

Status: slice 5 is built, all but one action. Built, in the library docs: the type schema on the wire and the
editor's settings form with Apply asking first ([control-port.md](../control-port.md#type-schemas),
[writing-a-node-type.md](../writing-a-node-type.md#the-type-schema), [editor.md](../editor.md#the-settings-form));
status fields, events, log codes, `Report`, the per-node queue, the tick and its paces, the `status` and `event`
records, node diagnostics and the feature `status` ([writing-a-node-type.md](../writing-a-node-type.md#status-and-events),
[architecture.md](../architecture.md#status-and-events), [output-records.md](../output-records.md#status),
[control-port.md](../control-port.md#status), [editor.md](../editor.md#status-events-and-diagnostics)); the realtime
channels and the bell ([architecture.md](../architecture.md#the-realtime-channels),
[writing-a-node-type.md](../writing-a-node-type.md#reporting-from-the-code), [engine.md](../engine.md#the-realtime-bell));
actions, triggers, calls, `node.invoke` and the editor's buttons ([writing-a-node-type.md](../writing-a-node-type.md#actions),
[control-port.md](../control-port.md#actions), [editor.md](../editor.md#actions)); `sendspin`'s notes split into status
and events ([node-types.md](../node-types.md#status-and-events)); and the test of ten chatty nodes and a slowly reading
client ([testing.md](../testing.md#the-client-queue-under-status)).

Slice 6 (parameters), designed together with slice 5 on this page until slices 3d and 5 were built, is built and in
the library docs; what it left is [`live-follow-ons.md`](live-follow-ons.md).

## The owner's words that bind what is left

| # | The owner's words | Date | What it means here |
|---|---|---|---|
| R9 | "Nodes should not have direct access to stdout or stderr. The core framework should expose the necessary primitives for events, diagnostics, and logging. …" | 2026-11-07 | A type reports through `Report` and `Io` only (built) |
| — | No sendspin action yet, and no `open-pairing-window` call (it would open pairing without the device gesture to any control-port client) | 2026-11-26 | Sendspin's first action waits for its library |

## What is left

### Sendspin's first action

`forget-pairing`, a call into the worker's loop ([writing-a-node-type.md](../writing-a-node-type.md#actions)) that
forgets one server's pairing (its record in the key file and the library's memory), so a server that lost its own side
can pair again without the node's identity being wiped. The pinned sendspin-cpp cannot serve it: its pairing API has no
way to forget one server's pairing. The owner decided (2026-11-26) that there is no sendspin action until a library can
forget one pairing, upstream or by a carried patch the owner chooses.

### Status coalesced in a client's queue

Not built, should a client ever need it: a node's `node.statusChanged` coalesced in the client's queue with that node's
own still queued, when only other nodes' status follows it (never past another message of the node, nor past another
notification). The literal "the last queued message" coalesces nothing with ten nodes interleaved. The measurement says
it is not needed yet ([testing.md](../testing.md#the-client-queue-under-status)).

### Reserved, not designed

`ear.realtime-overrun` (`codeUs`, `cycleUs`); `log.subscribe`; timestamped events; `writer::companion`, status a
companion writes, which is slice 8's ([`recovery.md`](recovery.md)); the `note` record's removal once `file` and
`airplay` move their notes to status and events.
