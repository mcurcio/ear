# Design docs: work not built yet

`docs/design` holds designs for future work only: each page here describes a slice that is not built, with the
owner's words that bind it, the problem, the design, what it changes in the built system, the decisions already taken
and what is still open. How ear works today is in the library docs (`docs/architecture.md`, `docs/engine.md`,
`docs/writing-a-node-type.md`, `docs/node-types.md`, `docs/graph-file.md`, `docs/control-port.md`,
`docs/output-records.md`, `docs/editor.md`), which these pages cite rather than repeat. When a slice is built, its page is deleted in
the same change, and whatever of it lasts (how the thing works, why it was built that way) moves into the library docs.
The owner's words keep the R, Q and G numbers of the register the framework's design used. A page cites only a number
its own tables define; what a retired page defined is said in place or pointed at in the library docs. Commits cite
the numbers too, and git keeps the pages they were on.

The slices are built in this order (decided: each later slice hooks into the earlier one's states, and companions,
built in slice 3b ([architecture.md](../architecture.md#companions)), removed primitives slice 3c would otherwise have built): 3c, 3d, then
5, 6, 8. `processing-nodes.md`'s speaker types (on its own branch) start after step 6.6 of slice 6
([parameters.md](parameters.md#11-order-of-work)).
Slice 3c is built but for the three pieces `slice-3c-remainder.md` keeps, none of which a later slice waits for. Slice 3d
(changes in place and reshapes) is built ([architecture.md](../architecture.md#changes-in-place),
[#reshapes](../architecture.md#reshapes)); the editor's Apply with a dry run came with slice 5's settings form
([editor.md](../editor.md#the-settings-form)), and a reshape in place of a node with companions is in
[architecture.md](../architecture.md#not-built-yet).

| Page | Slice | What it designs | Status |
|---|---|---|---|
| [`slice-3c-remainder.md`](slice-3c-remainder.md) | 3c | What slice 3c left unbuilt: the `sendspin` library's log lines attributed to their node by a carried patch (S3c.7), the converter companion's set and tracked modes (T5), and dropping the carried starved-floor patch | designed; S3c.7 waits on priorities, T5 on a first user, the patch's drop on evidence from both burst shapes; the rest of slice 3c is built and in the library docs |
| [`control-plane.md`](control-plane.md) | 5 | What slice 5 left: sendspin's first action (forgetting one pairing), status coalesced in a client's queue, and what is reserved | slice 5 is built and in the library docs but for that action, which waits on its library |
| [`parameters.md`](parameters.md) | 6 | Decimal, choice and indexed rows; one change mark per row (live ramped or stepped, in place, a restart with its reason: the owner's M3); slots and ramps in the realtime channels; retunes, mixed changes included; gestures with no control-thread timer; the `gain` type; the editor's live controls and faders | designed, revised on 2026-11-26 against slices 3d and 5 as built; open questions O1 to O7 for the owner |
| [`recovery.md`](recovery.md) | 8 | Failed nodes trying again by themselves with a backoff; `recovering` on the wire; status a companion writes | designed |
| [`cards-and-clock.md`](cards-and-clock.md) | later | The follower watchdog and a wedged card, sound cards opened off the main loop, a clock anchored to one card, latency reported while running, the hardware checks owed | partly designed |

Not built and not designed anywhere yet: routes naming a whole bank, a login on the control port, automatic card
retry beyond slice 8 and hotplug, arm64 images, `@include` and undo in the graph file, and changing the graph frequency
or block size while ear runs ([architecture.md](../architecture.md#not-built-yet) keeps the list); in the framework
itself, `node_host.cpp`'s size (about 4,100 lines) and whether each kind of resource should own its part of it behind
the readiness list, which wants its own page after reading the file whole, and whether `ear_node_type()` should
generate a type's one-function public header; the remaining `rtp`
items (a "receiving" status is slice 8's) and `sendspin` items are on their types' pages
([node-types.md](../node-types.md#rtp), [node-types.md](../node-types.md#sendspin)).
