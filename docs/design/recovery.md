# Recovery of failed nodes: slice 8 of the node framework

Status: design, not built. Slice 8 follows slice 3a (the lifecycle, built) and slice 5 (`control-plane.md`: node
diagnostics on the wire and `writer::companion` status). It makes a failed node try again by itself, with a backoff,
for as long as it is in the graph and not disabled. The built lifecycle, its failure codes and the lifecycle requests
are described in `docs/architecture.md` and `docs/control-port.md`.

[U] = unverified, with the test or check that settles it (section 7).

## 1. The owner's words that bind this slice

| # | The owner's words | Date | What it means here |
|---|---|---|---|
| Q4 | Default recovery: "Backoff, never gives up": 1 s doubling to 60 s, for as long as the node is in the graph | 2026-11-07 | The default policy for every type (3.1) |
| R8 | "Do we need to specify a hardware device for the clock? Can we drive it via an internal clock running under the raw monotonic clock, and just use a PLL (if necessary) to track against a hardware device anchor?" | 2026-11-07 | Built: the internal clock always drives and every card follows, so a node's failure and recovery cost only its own audio, which is what makes a recovery that never gives up safe for every type |
| R16 | "I like the basic shape of the state machine proposal. And most importantly: it should be per-node. Some node stopping/reconfiguring/failing does not effect the graph globally" | 2026-11-08 | Recovery is per node: one node's backoff never delays another's |
| R20 | "A “run” can’t fail — only a node can fail. ..." | 2026-11-08 | A failed node is torn down at once, keeps its block and wires, and is configured again from empty (built); recovery only decides when |
| R25 to R28 | "“Disabled” would be a better term"; "Keeps everything"; "Both words"; "Only disabled in file" | 2026-11-08 | A disabled node never retries and never recovers; a stop held on a failed node makes recovery rest it `stopped` |

## 2. What is built, and what is missing

As built (slice 3a): a failure (`engine-refused`, `start-failed`, `lost`) tears the node's configuration down at once,
keeping its block and wires; the node stays `failed` and is configured again **only when asked**: `node.start` (a
retry), `node.restart`, or a committed settings change (D-L31: rule 1 never configures a failed node by itself, "slice
8's recovery does that"). A lost block is final. So today a `sendspin` library that dies after an hour, or a USB card
unplugged and plugged back, stays failed until an operator acts or ear restarts.

## 3. The design

### 3.1 The state and its transitions

`NodePhase` and `NodeState` gain `recovering` (the wire's `state` and `phase` are open enums, so it is additive within
protocol 1). The transitions, in the framework's numbering:

| # | From | To | Trigger | What the framework does |
|---|---|---|---|---|
| T16 | `failed` | `recovering` | the failed configuration is gone (or leaked) and the policy allows another try | arms a main-loop timer for the wait |
| — | `recovering` | `configuring` | the wait ended | configures from empty with the newest settings, as a retry (cause `recovery`, with `attempt`) |
| T15 (built) | `failed`, `recovering` | `configuring` | a settings change, `node.start` or `node.restart` | at once, skipping the wait; the attempt count is kept |
| T21 (built) | `failed`, `recovering` | `disabled` | a disable | the recovery timer is cancelled |
| — | `recovering` | `stopping` (`removing`), then removed | the removal | the timer cancelled |

- **A lost block is final**: a node whose block was lost never enters `recovering` (it stays `failed`, `blockLost:
  true`, until it is removed and added or ear restarts).
- **A stop held on a failed or recovering node** (`node.stop`, built: it sets `wanted: "stopped"` and leaves the node
  failed) makes the next recovery rest it in `stopped` instead of `running`, as a retry does.
- **A disabled node never recovers**, and a node that boots disabled is never configured (built).
- **An enable that fails** is retried like any start that fails.

### 3.2 The waits

The first wait is keyed on **the phase the failure happened in**, never on the code's name:

- **After the node had rested** (`running` or `stopped`: a `lost`, or `engine-refused` for a companion lost after
  then): configured from empty again after **1 s**, doubling to at most **60 s**. A node that stays `running` for 60 s
  resets the wait to 1 s. A `sendspin` library that dies after an hour is tried again after 1 s.
- **Before the node rested** (`start-failed`, or `engine-refused` before then: a path that cannot be opened, a key file
  ear will not trust, a port another program holds, a card that never opens, a readiness deadline missed): the cause is
  in the settings or the host, so it retries at the **longest wait (60 s) from the first attempt**. It still never gives
  up, so a cause fixed by hand is picked up within a minute.

The waits are main-loop timers (`Timers`, with `now()`, built), one per node; one node's backoff never delays
another's.

### 3.3 A type's policy

The default is the framework's (3.2) for every type. A type may declare `recovery::none` or a cap on attempts, after
which the node stays `failed` until `node.start`, `node.restart` or a settings change. Decided here: the policy is a
member of the spec's `Lifecycle` struct (`reconfigure.md` 4: `Lifecycle{.reconfigureDeadline = ..., .recovery =
recovery::attempts(5)}`), so `TypeSpec`'s member order (`node-primitives.md` 6.11) does not change. The type schema
(slice 5) carries it as `recovery` on the type. No built-in type declares one.

### 3.4 Visible, not silent

- **`node.state`** (feature `recovery`): in `recovering`, `reason` (the failure that caused it, the built `{code,
  detail}`), `attempt` (from 1), `retryInMs`; in every phase after the first failure, `lastFailure` (`{code, detail,
  t}`), kept after the node runs again, so the editor can show "recovering, attempt 37, last: card lost". `reason` keeps
  its built meaning (present while `failed` and `recovering`).
- **`cause`** on the `configuring` a recovery starts: `{"kind":"recovery","attempt":n}`; a restart's `restart.kind` may
  be `recovery` too.
- **Standard output**: a `node` record when the node enters `recovering`:

  ```json
  {"v":1,"seq":812,"t":"2026-11-07T17:42:03.512094Z","kind":"node","node":"desk","state":"recovering","phase":"recovering","retryInMs":4000,"attempt":3,"failure":{"code":"engine-refused","detail":"card lost"},"msg":"node desk recovering in 4s (attempt 3): card lost"}
  ```

  and `node <n> starting: recovery attempt <n>` when the wait ends. (`failure` on the record and `reason` on the wire are
  one value under two names, as built.)
- **Diagnostics** (on the wire through slice 5's `node.diagnostic`, and as `diagnostic` records):
  `ear.recovery-attempt` (observation, `attempt`, `waitMs`), `ear.readiness-missed` (observation, subject the companion:
  a readiness deadline passed, `docs/engine.md#readiness-deadlines`), and `ear.health-flapping` (a condition: active when the node goes
  `running` → `failed` more than a threshold within a window, which catches a node that fails just after the backoff
  resets; inactive once a whole window passes at or below it).
- **The editor**: `recovering` has its own style; the box line counts down from `retryInMs` ("retry in 4 s (attempt 3):
  card lost"); actions: Start ("try now", a retry), Restart, Stop, Disable; type actions disabled (no configuration). A
  polite announcement when a node starts recovering, at most one per node per 5 s.
- **`node.restart`** on a `recovering` node is a retry (`applied: "retry"`), as on a `failed` one (built). A type action
  is refused `not-running` (slice 5).

### 3.5 Status a companion writes

Slice 5 defines `writer::companion`; this slice builds its first users:

- **`rtp`'s `receiving`** (a flag): the RTP receiver companion binds the rtp-source node's proxy and copies
  `rtp.receiving` (a bound proxy sees it; the registry does not), and passes a short `cleanup.sec` so silence is seen
  within seconds, not 60 [U: the smallest safe value]. It is status, not health: an `rtp` node with no sender is
  `running` and silent, as built.
- **A card's PCM state** (a choice) and whether its hardware pointer advances, from the follower watchdog's reading
  (`cards-and-clock.md` 3.1). A follower's rate correction lives inside the ALSA adapter, in no property ear can read,
  so it is not promised as status.

## 4. What it changes in the built system

| Built piece | Change |
|---|---|
| `NodePhase`, `NodeState`, the aggregate (`node_lifecycle.hpp`) | `recovering`; T16 and its end; the attempt count and last failure per node |
| `NodeHost` | a recovery timer per node; D-L31's rule changes: a failed node is configured again when the timer fires, as well as when asked |
| `NodeHealth` | `attempt`, `retryInMs`, `lastFailure` |
| `Lifecycle` (3d) | `recovery` |
| `ear_control` | feature `recovery`; the members; the `recovering` record and `recovery` cause |
| `web/` | `health.ts`'s `recovering` line, countdown and actions |

## 5. Tests

- `ear_core_tests` on the fake clock: the waits by phase (1 s doubling to 60 s after rest, 60 s from the first before
  rest); the reset after 60 s running; `node.start`, `node.restart` and a change skipping the wait with the count kept;
  a retune (slice 6) not skipping it; a disable cancelling it; a stop held making recovery rest `stopped`; a lost block
  never recovering; a type's cap; `ear.health-flapping` active and settling; the countdown's `retryInMs`.
- `test/protocol/lifecycle.json`'s recovery cases; `output-records.json`'s `recovering` record and diagnostics; vitest
  for the countdown.
- A system test: a `sendspin` node whose server goes away and comes back is not a failure (built: connection is status);
  a `card` node on the test card made to fail (its device removed) recovers on its own once the device is back; an `rtp`
  node on a port held by another socket fails, recovers every 60 s, and runs once the socket closes.

## 6. Decisions already taken

| # | Decision | Why |
|---|---|---|
| Q4 (owner) | Backoff, never gives up: 1 s doubling to 60 s | The owner's answer |
| — | The first wait is keyed on the phase of the failure: 1 s after the node rested, 60 s before | A node that ran and died is not a settings problem; one that never started is, and should not hammer the host |
| — | A disabled node never recovers; a disable cancels the wait | `disabled` releases everything and is the operator's word in the file |
| — | `node.start`, `node.restart` and a settings change skip the wait | The operator's intent wins at once |
| W2 | `reason` kept with its built meaning; `lastFailure` beside it | Additive within protocol 1 |
| new | A type's recovery policy is a member of `Lifecycle` | `TypeSpec`'s member order stays as `node-primitives.md` 6.11 gives it |

## 7. Open questions and risks

- **[U] Whether `snd_pcm_open` on a wedged USB device can block, and on which thread.** The ALSA adapter opens on ear's
  main loop inside the engine; with retries every 60 s the risk recurs. Slice 8 checks it on the MOTU (the
  `ear-on-talos` skill) before cards use recovery; if it can block, cards get `recovery::none` until sound cards are
  opened off the main loop (`cards-and-clock.md`).
- **Recovering forever is shown as it is**: an operator who does not want it disables the node.
- **A node that fails at a steady rate** (every 61 s) never resets past 60 s waits and writes a `node` record a minute;
  `ear.health-flapping` names it.
- The thresholds (1 s, 60 s, the 60 s reset, the flapping window) are first choices, to be tuned on the hardware.
