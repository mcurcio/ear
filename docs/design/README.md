# Design docs: work not built yet

`docs/design` holds designs for future work only: each page here describes a slice that is not built, with the
owner's words that bind it, the problem, the design, what it changes in the built system, the decisions already taken
and what is still open. How ear works today is in the library docs (`docs/architecture.md`, `docs/engine.md`,
`docs/writing-a-node-type.md`, `docs/node-types.md`, `docs/graph-file.md`, `docs/control-port.md`,
`docs/output-records.md`, `docs/editor.md`), which these pages cite rather than repeat. When a slice is built, its page is deleted in
the same change, and whatever of it lasts (how the thing works, why it was built that way) moves into the library docs.
The owner's words keep the R, Q and G numbers of the register the framework's design used, so commits and code
comments that cite them still resolve.

The slices are built in this order (decided: each later slice hooks into the earlier one's states, and companions
remove primitives slice 3c would otherwise build): 3b, 3c, 3d, then 5, 6, 8, 9.
`root-classes.md`'s steps are taken between slice 3b.4 and slice 3c, and `cycle-facts.md`'s after them, before 3c.
`timing-model.md` is part of slice 3c: its steps T1 to T3 come before S3c.1, T4 at any time, T5 after S3c.6.

| Page | Slice | What it designs | Status |
|---|---|---|---|
| [`companions.md`](companions.md) | 3b | The framework's hidden helpers for every crossing to non-realtime code: sinks and the new source served by their block in its cycle, cards and RTP receivers as engine companions with readiness deadlines, the quantum guard, `companions` on the wire and badges in the editor; its module design and headers | designed to module level; its first step is spike S5 on cycles an xrun cuts short |
| [`root-classes.md`](root-classes.md) | between 3b.4 and 3c | The architecture behind slice 3's fix clusters: one latency model in `ear_core` used by the block and the harness, one callback boundary for PipeWire's C callbacks, tests that induce what they wait for (and the suite's audit), one table of the PipeWire properties ear sets, and the harness on the engine's own rules | R1, R2, R4a, R4c, R5a, R5b and R6 built; the rest designed (Q-RC1 answered) |
| [`cycle-facts.md`](cycle-facts.md) | after the root classes, before 3c | Each cycle's facts stated once by the engine (cycles missed since a block last ran, which the driver cut, raw time lost, why a cycle is a discontinuity), the source, sink and `sendspin` moved onto them; one test judge that excuses exactly the cycles the driver cut and induces its own; diagnostics counters per graph and per node on the control port and standard output; a `SampleRate` type and compile-time refusals in place of the frame, callback and sleep guards | designed to module level; its first step is spike S6 on PipeWire's cut reports; Q-CF1 answered, Q-CF2 answered and then reversed by the owner's timing model (`timing-model.md`), Q-CF3 not asked, Q-CF4 answered; C1 to C4 and D1, D2 built, C4's `sendspin` and source timekeeping to be deleted by `timing-model.md` |
| [`timing-model.md`](timing-model.md) | 3c | The owner's timing model (one tick, one rate, clocks at the edges): sources that keep no time in the framework (`SourceClock`), sinks whose consumers reconcile, `sendspin`'s honest sink plus local skip with a clamped finish (C4's machinery and the floor patch deleted), and the converter companion (PipeWire's resampler between a ring at the node's rate and the graph, ratio set or tracked) | designed to module level; its converter's first step is spike S7; Q-TM1 and Q-TM2 open |
| [`node-primitives.md`](node-primitives.md) | 3c | The node handle, loops with their states, the I/O executor, stores, ear-wide claims, recordings opened off the main loop, the carried sendspin-cpp log patch; `sendspin` moved onto the type's class and the legacy adapter retired | designed |
| [`reconfigure.md`](reconfigure.md) | 3d | Changes in place (`.inPlace()` rows, the change version of `configure`, holds, make-before-break, the commit point, supersession, the fallback), reshape (`PortBlock::reshape`), `rtp` and `sendspin` in place, `node.set`, `dryRun` and `dropRoutes` | designed; its first step is a PipeWire spike (S4) |
| [`control-plane.md`](control-plane.md) | 5, 6 | The type schema on the wire, status, events, node diagnostics, actions and the realtime coded ring (5); decimal, indexed and live rows, parameter slots and ramps, retunes, gestures, the `gain` type and the editor's faders (6) | designed |
| [`recovery.md`](recovery.md) | 8 | Failed nodes trying again by themselves with a backoff; `recovering` on the wire; status a companion writes | designed |
| [`programs.md`](programs.md) | 9 | Outside programs joined through a pipe into a paced source, runtime files, the program resource; then the `airplay` type | sketched: the program supervisor and `airplay` need their own design |
| [`cards-and-clock.md`](cards-and-clock.md) | later | The follower watchdog and a wedged card, sound cards opened off the main loop, a clock anchored to one card, latency reported while running, the hardware checks owed | partly designed |

Not built and not designed anywhere yet: routes naming a whole bank, a login on the control port, automatic card
retry beyond slice 8 and hotplug, arm64 images, `@include` and undo in the graph file, and changing the graph frequency
or block size while ear runs ([architecture.md](../architecture.md#not-built-yet) keeps the list); the remaining `rtp`
items (a "receiving" status is slice 8's) and `sendspin` items are on their types' pages
([node-types.md](../node-types.md#rtp), [node-types.md](../node-types.md#sendspin)).
