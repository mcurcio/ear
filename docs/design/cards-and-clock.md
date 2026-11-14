# Sound cards and the clock: what is still to build

Status: partly designed, not built, no slice number yet. The internal clock (the dummy driver, always driving, tracking
`CLOCK_MONOTONIC_RAW`), cards following it with adaptive resampling, and latency carried per bank pair are built
(slice 7; `docs/engine.md`). This page holds the clock and card work the framework design left for later: a follower
watchdog for a frozen card (designed, 3.1), what ear does with a wedged card (designed, 3.2), sound cards opened off the
main loop (constrained, not designed, 3.3), an internal clock anchored to one card (sketched, 3.4), latency a node
reports while it runs (3.5), and the checks still owed on real hardware (4). It builds on cards as engine companions
(built, `docs/engine.md#engine-companions`) and needs slice 3c (the I/O executor, `node-primitives.md`).

[V] = verified, [U] = unverified, each with what settles it.

## 1. The owner's words

| # | The owner's words | Date | What it means here |
|---|---|---|---|
| R7 | Outputs on different devices stay aligned, for "seperate zones" | 2026-10-29 | Latency and clock facts belong to the audio contract; the anchor (3.4) is for a zone that must be bit-perfect |
| R8 | "Do we need to specify a hardware device for the clock? Can we drive it via an internal clock running under the raw monotonic clock, and just use a PLL (if necessary) to track against a hardware device anchor?" | 2026-11-07 | Built: the internal clock. Its second half, tracking a hardware anchor, is 3.4 |
| Q3 | "Internal, cards follow" | 2026-11-07 | Built |
| Q8 | "Track the raw clock" | 2026-11-07 | Built (`clock.id = monotonic-raw`) |
| R16 | "... it should be per-node. Some node stopping/reconfiguring/failing does not effect the graph globally" | 2026-11-08 | A wedged card is the one place isolation does not hold today (3.2, 3.3) |

## 2. What is built, and the known limit

A card that cannot open or is lost makes its node `failed engine-refused: <detail>` and moves no clock (built). But
nothing in the graph shows a **frozen follower**: it still takes the block's buffer every cycle, so a card whose USB
stream stalls without an error looks fine. And ALSA's open, prepare, drop and close run on ear's main loop inside the
engine, synchronously, so **a USB card that wedges inside the kernel can block `snd_pcm_drop` or `snd_pcm_close`, and
with them every node, the control port's graph requests and the keeper's main-loop side, until ear restarts** [U:
whether a wedged MOTU blocks either]. The main-loop watchdog (built, `ear.main-loop-stalled`) reports such a stall while
it lasts; it cannot end it.

One symptom of the frozen follower is already seen on the test card, unexplained: `sound_card_test.cpp`'s "to_card's
last 5 ms before a silence decay to zero" failed about 1 run in 50 beside 5 busy processes on 4 cores (found 2026-11-10,
at a317bcc305 too), its card's file not growing by 16 blocks within 5 s of the drive. Why the card stops writing is not
known [U: whether it still fails since the card tests moved onto the cycle judge, 0b055fd638; if so, the follower's
state and the PCM's pointer at the deadline, which 3.1's watchdog would read].

## 3. The design

### 3.1 The follower watchdog

- **The signal**: the PCM's state and hardware pointer from `/proc/asound/<card>/pcm<n><p|c>/sub0/status`, read on the
  I/O executor every few hundred milliseconds per card companion. A pointer that does not advance while the state is
  RUNNING, for a bound, is a fact against that card alone [U: that the file shows a wedged USB card as stalled; checked
  on the MOTU].
- **The bound** sits well above the stalls a loaded host's data thread already shows (hundreds of milliseconds, xruns),
  so a loaded host is never judged a wedged card; a first choice of 2 s, tuned on the hardware.
- **Testable**: `/proc/asound` is absent in the dev container and CI, and the test card lives inside alsa-lib with no
  `/proc` entry [V], so the watchdog's PCM-status source is injectable and CI fakes it.
- **Status** (slice 8, `recovery.md` 3.5): the card companion writes the PCM's state and whether its pointer advances as
  `writer::companion` fields, so the editor can show a card that is open but not moving.

### 3.2 A card judged wedged

Acting on the watchdog **never calls into ALSA**: destroying the card's links or node would drop or close the PCM on the
main loop, which a wedged USB device can block. So a card judged wedged is never called into again: its node fails
`engine-refused: card <id> wedged`, its companion is leaked (neither its links nor its node destroyed), and it is counted
as a leaked configuration is (built: `NodeHost::leaked()`, and ear's stop leaves through `std::_Exit`). Configuring the
node again opens the device afresh, which fails while the leaked one holds it, until ear restarts; so slice 8's recovery
(`recovery.md`) does not retry a node failed as wedged, unless 3.3 is built. The operator is told plainly: the failure's detail says the card is wedged
and that ear must restart to use it again.

### 3.3 Sound cards opened off the main loop (not designed)

The fix for 2's limit is to move ALSA's open, prepare, drop and close for a card companion off ear's main loop, so a
wedged USB card blocks only its node. Two directions, to be chosen once the MOTU check says whether a wedged card
blocks those calls at all:

1. **A PipeWire context or loop of their own for card adapters**, so the adapter's synchronous ALSA calls run on a
   thread that is not ear's main loop. Constraint: every block and companion shares one data loop (built,
   `context.num-data-loops = 1`), and the adapters must still follow the dummy driver.
2. **ear's own ALSA I/O feeding a ring companion**: a thread per card that owns the PCM, writing into a source and
   reading from a sink (`docs/engine.md#sources`, `#sinks`), with its own rate matching against the graph clock. More code, but every ALSA
   call is then on a thread ear can abandon.

Either way a wedged card's thread is leaked, never joined, and the node fails; the rest of the graph is untouched.

### 3.4 An internal clock anchored to one card (sketched)

R8's second half: an internal clock that tracks one card (that card bit-perfect, every other card resampled to it) and,
when the card is lost, keeps its last rate so the graph never stops. It needs ear's own driver node (the dummy driver
cannot be steered by a card); it can be added as a graph setting, `clock anchor desk;`, without changing the model
(every card still follows, the anchor's follower loop becomes the graph's). Not designed: the driver node, how the
anchor's rate estimate is filtered, and what happens to the anchor's own latency.

### 3.5 Latency a node reports while it runs

Latency is declared once, when a configuration is made (built: `cx.declareLatency`, a companion's on its link). A node
whose latency changes while it runs (a network jitter buffer that grows) has no way to say so. A dynamic report (a call
on the node handle, carried by the framework on the next cycle, as an in-place change's plan is) is owed; not designed.

## 4. Checks owed on real hardware (the MOTU 24Ao, the `ear-on-talos` skill)

- [U] A real USB card at `quantum-limit` = the block size (`docs/engine.md#the-quantum-guard`; moved here from slice
  3b's page when it was built). Proven on the test card and on RTP receivers at 44.1 and 96 kHz; if the MOTU will not
  run at that limit, the limit goes back to `max(block, 8192)` and the refusal of force and lock properties stays the
  guard.
- [U] Drift tracking of a following card, and follower resyncs at a 256-frame quantum: USB cards report their pointer
  at URB granularity, so the follower's "delay too big" resync can fire and click; `api.alsa.headroom` and the period
  size are the knobs.
- [U] CPU for 24 resampled channels on the Talos nodes' CPU.
- [U] Whether a card's playback and capture, two ALSA adapters each following with its own rate match, drift apart
  (round trips and echo cancellation across them are out either way).
- [U] What `/proc/asound/.../status` shows for a wedged USB card (3.1); whether a USB unplug reaches the adapter's ERROR
  state without udev in the container; whether `snd_pcm_open`, `snd_pcm_drop` or `snd_pcm_close` can block on a wedged
  card (decides 3.3).
- [U] Following a host clock slewed at chrony's worst case (83,333 ppm) through the dummy's 1 ms-a-cycle clamp.
- A pause of the data thread of tens of seconds or more still slews the graph off the raw clock for minutes (built
  `resync.ms` covers stalls of a few seconds); the fix is upstream's (PipeWire's `node-driver.c` would have to re-arm
  its timer and its loop on a reset), checked at each PipeWire bump.

## 5. Decisions already taken

| # | Decision | Why |
|---|---|---|
| D-R2 | A wedged sound card is a known limit of isolation; a card judged wedged is never called into again (its companion leaked); opening cards off the main loop is a later piece of work | Destroying a wedged card's links or node would call the blocking ALSA functions; saying so lets the operator restart ear knowingly |
| — | The watchdog's source is injectable | CI and the dev container have no `/proc/asound` |
