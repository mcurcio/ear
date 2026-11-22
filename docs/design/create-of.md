# What `configure` asks and answers: a smaller, stricter `CreateOf`

Status: designed, not built; approved by the owner (2026-11-22: Q-C1 to Q-C3, section 10). It follows slice 3c (one author interface is left, the class:
`docs/writing-a-node-type.md#the-class`) and comes before slice 3d (`reconfigure.md`), which adds the change version
of `configure` on the same surface. It changes how a type's `configure` is written and nothing an operator sees: no
setting, state, record or wire member changes.

[U] = unverified, with the check that settles it (section 8).

## 1. The owner's words that bind this page

| # | The owner's words | Date | What it means here |
|---|---|---|---|
| R1 | "what are the core primitives that ear should host so that new nodes can be added simply and only require defining the truly custom bits that its node type demands" | 2026-11-07 | A type's `configure` says what the type needs and nothing about how the framework keeps count |
| R33 | Asked "Should configure come in two versions, each offering only its legal calls, so a wrong call won't compile?": "Two versions" | 2026-11-08 | The standard this page holds `CreateOf` to: a wrong `configure` does not compile, where a compiler can tell |
| — | "I'm always interested in architectural improvements that simplify and harden the framework with a goal of elegant developer experience." | 2026-11-21 | The reason for the page |

## 2. What is built, and what is wrong with it

`configure` is handed a `CreateOf<spec>`, makes calls on it in any order, and returns `Configured::applied()` or
`Configured::failed(detail)` (`docs/writing-a-node-type.md#configure-and-createof`). Banks, loops and stores are
checked against the spec at compile time. Three things are not.

**A. Rules that are only counts are checked at run time.** Each of these is "exactly once", and the compiler could
hold every one of them:

| Rule | Today, when broken | Rows of `#rules-the-framework-enforces` |
|---|---|---|
| The code is given exactly once | `configure threw: use called twice`; `configure gave no code` | 2 |
| A loop declared with a state is given one, once | `configure gave loop <name> no state`; `configure threw: loop <name> given a state twice` | 1 |
| A sink has exactly one consumer | `configure gave sink <bank> no consumer`; `configure threw: consumedBy called twice on sink <bank>` | 1 |
| A program that names a bank's FIFO has a pipe receiver on that bank | `configure gave program <name> the FIFO of bank <bank>, which has no pipe receiver` | 1 |
| A pass-through names at least one pair | `configure threw: use given no code: ...` | 1 (shared with a null code) |

A type author meets each as a failed node in a test, not as an error in the editor. The framework pays for each with
a run-time check in `node_host.cpp`, its text, a row in the docs and a row or more in `node_host_test.cpp`.

**B. Two calls are split in two for no reason a type has.** `cx.sink(bank, shape)` returns a `SinkOf` whose only
purpose is to be told its consumer by `.consumedBy(...)`; the sink is useless between the two. `cx.fifoOf(bank)` names
a FIFO that exists only if `cx.pipeReceiver(bank, ...)` is also called.

**C. One method per kind of companion.** `soundCard`, `rtpReceiver`, `pipeReceiver`, `sink` and `source` each repeat
the same compile-time checks (the spec's bank, its reach, its direction) and the same run-time rule (one companion per
bank). `CreateContext` repeats the three engine kinds, and slice 3d's `ChangeOf` is designed to repeat all five
(`reconfigure.md` 4). Inside `NodeHost` the three engine kinds are already one thing (`EngineAsk`, `engineKind`). A
new kind (an RTP sender, the set and tracked converters) adds a method to each of the three contexts.

## 3. The design in one paragraph

`configure` **attaches** companions to banks with one call whose shape says the kind, and **answers with the code and
its loops' states**: `return cx.applied(code, states...)`. What must be given exactly once is an argument of the one
call that needs it, so giving it twice or not at all does not compile. The class, the spec, settings, banks, loops,
stores, claims, recordings and programs are otherwise as built.

## 4. The design

### 4.1 The answer carries the code and the loops' states

```cpp
Configured configure(CreateOf<spec>& cx, const Checked&) {            // white
	return cx.applied(WhiteCode{});
}

Configured configure(CreateOf<spec>& cx, const Checked& s) {          // card
	if (s.get<playbackRow>() > 0) cx.attach(toCard, SoundCardShape{.card = s.get<cardRow>()});
	if (s.get<captureRow>() > 0) cx.attach(fromCard, SoundCardShape{.card = s.get<cardRow>()});
	return cx.applied(passThrough(bankPair(play, toCard), bankPair(fromCard, capture)));
}
```

- **`cx.applied(code, states...)` is the only way to make an applied answer.** `Configured::applied()` goes;
  `Configured::failed(detail)` stays. `cx.use` goes. So "no code" and "the code twice" cannot be written.
- **A loop's state is made by the context and given in the answer.** `cx.state(loop, args...)` constructs the loop's
  `State` from `args` and returns a `LoopState<loop>`: move-only, never null, made only there. `cx.applied` takes
  one for every loop the spec declares with a state, each once, in any order; a missing one, a second one, or one for
  a loop declared without a state does not compile. `cx.loop(handle, std::unique_ptr<State>)` goes.

  ```cpp
  auto work = cx.state(worker, WorkerParts{.handle = node_.handle(), .keys = cx.store(keyFile), ...});   // sendspin
  auto notify = cx.state(notifier, cx.executor(notifier), clock, player_);
  cx.post(worker, [](Worker& state) { state.begin(); });      // staged, as built: the state need not be given yet
  cx.attach(toServer, SinkShape{.frames = half}, worker, &Worker::toServer);
  return cx.applied(passThrough(bankPair(fromServer, from), bankPair(to, toServer)), std::move(work), std::move(notify));
  ```

  Nothing in `configure` needs a state before the answer: a post is staged, and a sink's consumer and a program's feed
  name a loop and a member, never the state itself (as built).
- **`passThrough(first, rest...)`** takes its pairs as arguments, at least one, so an empty pass-through cannot be
  written.
- **A code is given by value** (a struct with `process`, or a pass-through). The `std::unique_ptr<AudioCode>` form
  stays for the erased layer and its tests and leaves `CreateOf`, so no type can give a null code [U1].
- The answer is made by the context because the checks need the spec. A `Configured` is still only an answer: making
  one and not returning it has no effect.

### 4.2 One call for a companion: `attach`

A companion is what joins one inside bank of the node. The rule a type learns is one: **one companion per bank,
attached by its shape.**

| Call | Bank | Returns |
|---|---|---|
| `cx.attach(bank, SoundCardShape{...})` | inside, either direction (the bank's is the card's) | nothing |
| `cx.attach(bank, RtpReceiverShape{...})` | inside input | nothing |
| `part::Fifo fifo = cx.attach(bank, PipeReceiverShape{...})` | inside input | the receiver's FIFO, for a program's argument or file |
| `SourceWriter& w = cx.attach(bank, SourceShape{...}, clock = nullptr)` | inside input | the writer |
| `cx.attach(bank, SinkShape{...}, consumer)` | inside output | nothing |

- **A sink's consumer is an argument**, in the three forms `consumedBy` has today: a `std::unique_ptr<SinkConsumer>`,
  `loop, &State::member`, or `loop` alone. `SinkOf`, `SinkRequest` and `consumedBy` go. A sink without a consumer
  cannot be written. A null `unique_ptr` stays a run-time rule (`configure threw: sink <bank> given no consumer`).
- **A pipe receiver's FIFO is what attaching it returns**, so `fifoOf` goes and a program cannot name the FIFO of a
  bank with no receiver.
- **Each shape states its bank rule once**, as a constant the one `attach` reads (`companionBank<Shape>`: the reach,
  and the direction or "either"). A shape with no rule is not a companion and does not compile. The compile-time
  errors keep today's texts ("a sound card joins an inside bank of the spec", and so on).
- **`CreateContext` gets one call for the engine kinds**, `attachEngine(bank, EngineShape)`, a variant of the three
  shapes, which `NodeHost` already holds as one (`EngineAsk`). Sinks and sources keep their own erased calls: each
  returns or takes something of its own.
- "A second companion on a bank" stays a run-time rule: which banks a `card` attaches to depends on its settings.

*Why one verb for all five, not only the engine kinds:* the rule and the checks are the same five times, and a type
author does not need the distinction between an engine companion and a ring companion to write `configure`; the docs
keep it where it matters (`docs/engine.md#engine-companions`, `#sinks`, `#sources`).

### 4.3 What does not change

`cx.format()`, `cx.node()`, `cx.recording(...)`, `cx.declareLatency(from, to, latency)`, `cx.executor(loop)`,
`cx.post(loop, work)`, `cx.store(handle)`, `cx.claim(...)` and `cx.program(...)` are as built. Each either may be
called any number of times or carries a rule that depends on settings. `NodeOf`, the spec, `ports()`, `check`, `start`
and `stop` are untouched.

### 4.4 `CreateOf` before and after

| Before (17 calls and an answer) | After (11 calls and an answer) |
|---|---|
| `use` | in the answer |
| `loop` | `state`, given in the answer |
| `soundCard`, `rtpReceiver`, `pipeReceiver`, `source`, `sink` + `SinkOf::consumedBy` ×3 | `attach` |
| `fifoOf` | what `attach` returns |
| `format`, `node`, `recording`, `declareLatency`, `executor`, `post`, `store`, `claim`, `program` | the same |
| `Configured::applied()`, `Configured::failed(detail)` | `cx.applied(code, states...)`, `Configured::failed(detail)` |

Run-time rules that leave `docs/writing-a-node-type.md#rules-the-framework-enforces`: `use` twice; no `use`; a sink
with no consumer or two; a loop given no state or two; a program naming the FIFO of a bank with no pipe receiver; an
empty pass-through. Six rows become compile-time refusals, each with a case in `type_spec_bad.cpp`.

### 4.5 The erased layer

`ErasedInstance::configure(CreateContext&)` answers an erased `Configured` that holds the code and the states (one per
declared stateful loop, by index). `CreateContext::use` and `loopState` go; `sink` takes its consumer. `SpecInstance`
makes the erased answer from `cx.applied`, so it is right by construction; `NodeHost` keeps one check of it (the
states match the declared loops), as its own bug if it ever fails (`ear.internal-error`, as an adapter's
`std::logic_error` is: `docs/architecture.md#faults-by-owner`). `NodeHost`'s `codeGiven` flag, its two sink-consumer
checks, its loop-state checks and the FIFO check at `configure`'s return go, with their rows in `node_host_test.cpp`.

### 4.6 The change version

`reconfigure.md` 4 designs `ChangeOf` with the built names. When this page is built that section changes with it, so
slice 3d is built once, on this surface:

- Replacements are `cx.attach(bank, shape, ...)`, as on creation; `Order` stays the sound card's extra argument.
- A sink's replacement takes its consumer as an argument.
- The code is in the answer: `cx.applied()` keeps the code, `cx.applied(code)` swaps it; `Changed::restart(reason)`
  stays. "A change must give the code when the ports changed" stays a run-time rule (it depends on settings).
- `whenReady` still returns the one later answer.

`ChangeOf` has no `state`: a loop's state lives across a change (`reconfigure.md` 6.3).

## 5. What it changes in the built system

| Where | Change |
|---|---|
| `ear/core/lifecycle.hpp` | `CreateOf`: `attach`, `state`, `applied`; `use`, `loop`, `soundCard`, `rtpReceiver`, `pipeReceiver`, `source`, `sink`, `fifoOf` go; `SinkOf` and `SinkRequest` go; `LoopState<Handle>`; `Configured` holds the erased answer; `CreateContext` as 4.5 |
| `ear/core/type_spec.hpp` | `passThrough(first, rest...)`; `companionBank<Shape>` beside each shape |
| `src/core/node_host.cpp` | The checks 4.5 names go; the answer's code and states are taken at `configure`'s return |
| `src/nodes/*` | Each type's `configure` rewritten (six types; 4.1's examples are `white`, `card` and `sendspin` whole) |
| `ear/testing/*` | The harness's fakes follow `CreateContext`; `TypeHarness` is unchanged for a type's tests |
| `src/core/tests/` | The test classes' `configure`s rewritten (about 110 `use` sites, 40 `consumedBy`, 75 companion calls: mechanical); the run-time rows of 4.4's rules deleted; compile-fail cases added (6) and renamed (the per-kind bank cases keep their texts) |
| `docs/writing-a-node-type.md` | `#configure-and-createof`'s table, `#rules-the-framework-enforces`, `#sinks`, `#loops`, `#programs`, the `tone` example |
| `docs/design/reconfigure.md` | Section 4, as 4.6 |

Nothing changes in `ear_pipewire`, `ear_runtime`, `ear_control`, `ear_config`, the wire, the fixtures or the editor.

## 6. Tests

- **Compile-fail, new** (`type_spec_bad.cpp`): an answer with no code (there is no call to write: the case is
  `Configured::applied()`, which no longer exists); a stateful loop's state missing from `cx.applied`; one given
  twice; a state for a loop declared without one (exists, re-pointed at `cx.state`); a sink attached with no consumer;
  `passThrough()` with no pair; a shape that is no companion given to `attach`.
- **Compile-fail, kept:** each companion on the wrong bank, with today's texts; a loop or store the spec lacks; a post
  of a function of another loop's state.
- **Unit:** every shipped type's own tests pass unedited but for their test classes' `configure`s, if any: the
  harness's view of a type (its companions, sinks, sources, loops, programs) is the same.
- **`node_host_test.cpp`:** the rows for the six run-time rules that left are deleted; one new row for 4.5's internal
  check (an erased instance that answers the wrong states fails its node `lost`, `internal error`, with an
  `ear.internal-error` record).
- **Proof that nothing an operator sees changed:** every system test (`test/system/`) and `test/nodes/` passes
  unedited, and `test/protocol/` is untouched.

## 7. Order of work

Each step builds and passes alone; each is a `refactor(core)` commit and makes no release.

| Step | What | Why this order |
|---|---|---|
| C1 | A sink's consumer as an argument; `SinkOf`, `SinkRequest`, `consumedBy` go | The smallest; no new name |
| C2 | `attach` for the five kinds; a pipe receiver's FIFO returned; `fifoOf` and the per-kind calls go; `attachEngine` | Renames C1's call once more, but keeps C1 reviewable alone |
| C3 | The answer: `cx.applied(code, states...)`, `cx.state`, `passThrough(first, rest...)`; `use`, `loop`, `Configured::applied()` go; the erased answer | The widest (every `configure`), last so it lands on the final names |
| C4 | `reconfigure.md` 4 rewritten on this surface; this page deleted, what lasts moved to `docs/writing-a-node-type.md` | With C3 |

## 8. Unverified

- **[U1]** That no shipped type and no type test needs to give its code as a `std::unique_ptr` (a code too large to
  move, or one made by a factory). Settled at C3 by removing the constructor from `NodeCode`'s author-facing forms and
  building; if one does, the form stays and "a null code" stays a run-time rule.
- **[U2]** That `cx.applied`'s pack check gives an error a type author can read (which loop's state is missing) under
  GCC 14. Settled at C3 by the compile-fail cases' expected texts; if the text is poor, each missing loop gets its own
  `static_assert` through a fold.
- **[U3]** That `cx.state(loop, args...)` constructing in place loses nothing against `std::make_unique<State>(...)`
  for `sendspin`'s and `airplay`'s states (aggregate parts, references). Settled at C3 by porting both.

## 9. Decisions and what was set aside

| Decision | Why |
|---|---|
| The code and the states are arguments of the answer | "Exactly once" is what an argument is; no count is kept anywhere |
| The context makes the answer | The checks need the spec; a static `Configured::applied(...)` could not see it |
| States are made by `cx.state`, not passed as `std::unique_ptr` | Never null, and tagged with their loop, so the answer's check is on types alone |
| One verb, `attach`, for all five companions | One rule (a companion per bank) and one set of checks, stated once per shape |
| "A second companion on a bank" stays at run time | Which banks are attached depends on settings (`card`) |
| The erased layer changes too | Otherwise the run-time checks, their texts and their tests stay for a rule no type can break |

| Set aside | Why |
|---|---|
| Declaring companions in the spec (`TypeSpec`), with nothing to call in `configure` | A companion's presence and shape depend on settings (`card`'s directions, `rtp`'s address); the spec is fixed at compile time |
| A typestate `CreateOf` (each call returns a context of a new type, the answer only from the last) | Linear types are not C++'s: a moved-from context still compiles, and every `configure` becomes a chain |
| A builder (`cx.code(...).state(...).build()`) | The same counts, kept in the builder; nothing is refused earlier |
| Keeping `soundCard`, `rtpReceiver`, `pipeReceiver` as names over one erased call | Halves the gain: `ChangeOf` and every later kind still add a method each |
| Leaving `CreateOf` as it is until after slice 3d | Slice 3d designs `ChangeOf` as its twin; building 3d first means writing and migrating the change surface twice |

## 10. Questions for the owner

**Q-C1. One verb for every companion, or only for the engine kinds?** **Answered (the owner, 2026-11-22, binding):
one verb, `attach`, for all five.** *Recommendation: one verb, `attach`, for all
five* (4.2). The alternative keeps `cx.sink(bank, shape, consumer)` and `cx.source(bank, shape, clock)` under their
own names and uses `attach` for sound cards, RTP receivers and pipe receivers only: two more names to learn, and the
calls read a little more like prose.

**Q-C2. Is `cx.applied(code, states...)` the right spelling of the answer?** **Answered (the owner, 2026-11-22,
binding): yes.** *Recommendation: yes.* It keeps the word
the answer has today and reads as what it is. The alternative names the code's verb, `cx.run(code, states...)`, which
reads well for creation and badly for slice 3d's change (`cx.run()` to mean "keep the code").

**Q-C3. Before slice 3d, as section 7 has it?** **Answered (the owner, 2026-11-22, binding): yes, before slice 3d.**
*Recommendation: yes*, for the reason in section 9's last row. It
delays slice 3d's spike by the four steps above.

## 11. Not in this page

- `node_host.cpp`'s size (about 4,100 lines after slice 3c) and whether each resource kind should own its part of it
  behind the readiness list. Section 4.5 removes a little; the rest wants its own page, after reading the file whole.
- A type's five files: whether `ear_node_type()` should generate the one-function public header.
