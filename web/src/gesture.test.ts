// A fader's gesture against a fake client (docs/editor.md#gestures-in-the-editor): every edge of the state table, blur
// and a hidden window letting go, a pointer press during a keyboard gesture continuing it, the hidden range input's own
// keys as a gesture, a lost connection freezing it and the reconnect ending it by its id, and the notices.
import { describe, expect, it } from "vitest";
import { GestureControls, keepAliveMs, keyEndMs, noticeWaitMs, pendingMs, type BeginAnswer } from "./gesture";
import { inputValueOf, rangeKey, valueOfInput } from "./live";
import { isTextTarget, shortcutOf } from "./keys";
import type { NoticeInput } from "./notices";
import type { Graph, GraphNode, Notifications, SettingSchema } from "./protocol";
import { ConnectionClosedError, RpcError } from "./rpc";

const gainDb: SettingSchema = {
  key: "gainDb",
  kind: "decimal",
  required: false,
  change: "live",
  live: { transition: "ramp", ms: 20 },
  default: 0,
  min: -90,
  max: 24,
  unit: "dB",
  decimals: 1,
  taper: "db",
  minIsSilence: true,
};

interface Deferred<T> {
  promise: Promise<T>;
  resolve(value: T): void;
  reject(error: unknown): void;
}

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((yes, no) => {
    resolve = yes;
    reject = no;
  });
  return { promise, resolve, reject };
}

/** Lets every settled promise's handlers run. */
const settle = () => new Promise<void>((done) => setTimeout(done, 0));

const refusal = (code: string, message: string, data: Record<string, unknown> = {}) =>
  new RpcError({ code: -32000, message, data: { code, ...data } as never });

/** A session's controls over a fake client, fake timers and frames, and a graph the test sets. */
function harness() {
  const sent: Array<{ method: string; params: Record<string, unknown> }> = [];
  const begins: Deferred<BeginAnswer>[] = [];
  const ends: Deferred<{ value: number }>[] = [];
  const cancels: Deferred<unknown>[] = [];
  const timers: Array<{ at: number; run: () => void; live: boolean }> = [];
  const frames: Array<{ run: () => void; live: boolean }> = [];
  const notices: NoticeInput[] = [];
  const instead: Array<{ name: string; key: string; value: number }> = [];
  let now = 0;
  const node: GraphNode = {
    name: "amp",
    type: "gain",
    settings: { gainDb: -6 },
    inputs: [],
    outputs: [],
    phase: "running",
    gestures: [],
  } as unknown as GraphNode;
  let graph: Graph = { nodes: [node], routes: [] } as unknown as Graph;
  const controls = new GestureControls(
    {
      begin: (name, key) => {
        sent.push({ method: "gesture.begin", params: { name, key } });
        const call = deferred<BeginAnswer>();
        begins.push(call);
        return call.promise;
      },
      change: (gesture, seq, value) => void sent.push({ method: "gesture.change", params: { gesture, seq, value } }),
      end: (gesture, value) => {
        sent.push({ method: "gesture.end", params: value === undefined ? { gesture } : { gesture, value } });
        const call = deferred<{ value: number }>();
        ends.push(call);
        return call.promise;
      },
      cancel: (gesture) => {
        sent.push({ method: "gesture.cancel", params: { gesture } });
        const call = deferred<unknown>();
        cancels.push(call);
        return call.promise;
      },
    },
    {
      after: (ms, run) => {
        const timer = { at: now + ms, run, live: true };
        timers.push(timer);
        return () => void (timer.live = false);
      },
      frame: (run) => {
        const frame = { run, live: true };
        frames.push(frame);
        return () => void (frame.live = false);
      },
    },
    {
      graph: () => graph,
      countRowOf: (_name, key) => (key.startsWith("trimDb.") ? { by: "channels", default: 2 } : undefined),
    },
    (notice) => notices.push(notice),
    (name, key, value) => instead.push({ name, key, value }),
  );
  const control = controls.control("amp", "gainDb", gainDb);
  const answer = (value = -6, gesture = "g1"): BeginAnswer => ({ gesture, value, min: -90, max: 24, decimals: 1 });
  return {
    controls,
    control,
    sent,
    begins,
    ends,
    cancels,
    notices,
    instead,
    answer,
    methods: () => sent.map((one) => one.method),
    /** Moves the clock by `ms`, running each timer due. */
    advance(ms: number) {
      now += ms;
      for (const timer of [...timers]) {
        if (timer.live && timer.at <= now) {
          timer.live = false;
          timer.run();
        }
      }
    },
    /** Runs the animation frames requested so far. */
    frame() {
      for (const frame of frames.splice(0)) if (frame.live) frame.run();
    },
    setGraph(change: (node: GraphNode) => GraphNode) {
      graph = { ...graph, nodes: [change(graph.nodes[0]!)] };
    },
    changed(params: Notifications["gesture.changed"]) {
      controls.apply({ jsonrpc: "2.0", method: "gesture.changed", params } as never);
    },
  };
}

/** A harness whose gesture g1 is active, begun from -6. */
async function active() {
  const h = harness();
  h.control.press();
  h.begins[0]!.resolve(h.answer());
  await settle();
  expect(h.control.get().state).toBe("active");
  return h;
}

describe("a gesture", () => {
  it("idle: a press begins it, showing the node's value", () => {
    const h = harness();
    h.control.press();
    expect(h.sent).toEqual([{ method: "gesture.begin", params: { name: "amp", key: "gainDb" } }]);
    expect(h.control.get()).toMatchObject({ state: "beginning", value: -6, pending: false });
  });

  it("idle: a first key begins it too", () => {
    const h = harness();
    h.control.nudge(-5);
    expect(h.methods()).toEqual(["gesture.begin"]);
    expect(h.control.get()).toMatchObject({ state: "beginning", value: -5 });
  });

  it("beginning: shown pending after 200 ms with no answer", () => {
    const h = harness();
    h.control.press();
    h.advance(pendingMs - 1);
    expect(h.control.get().pending).toBe(false);
    h.advance(1);
    expect(h.control.get()).toMatchObject({ state: "beginning", pending: true });
  });

  it("beginning: a move keeps the newest and sends nothing, then the answer sends it as tick 1", async () => {
    const h = harness();
    h.control.press();
    h.control.move(-5);
    h.control.move(-4);
    expect(h.methods()).toEqual(["gesture.begin"]);
    h.begins[0]!.resolve(h.answer());
    await settle();
    expect(h.sent[1]).toEqual({ method: "gesture.change", params: { gesture: "g1", seq: 1, value: -4 } });
    expect(h.control.get()).toMatchObject({ state: "active", id: "g1", value: -4, range: { min: -90, max: 24, decimals: 1 } });
  });

  it("beginning: an answer with no move sends no tick", async () => {
    const h = await active();
    expect(h.methods()).toEqual(["gesture.begin"]);
    expect(h.control.get().value).toBe(-6);
  });

  it("beginning: a release before the answer ends at the shown value once answered", async () => {
    const h = harness();
    h.control.press();
    h.control.move(-3.5);
    h.control.release();
    expect(h.control.get().state).toBe("beginning");
    h.begins[0]!.resolve(h.answer());
    await settle();
    expect(h.sent[1]).toEqual({ method: "gesture.end", params: { gesture: "g1", value: -3.5 } });
    expect(h.control.get().state).toBe("ending");
  });

  it("beginning: Escape before the answer cancels once answered", async () => {
    const h = harness();
    h.control.press();
    h.control.move(-3.5);
    h.control.escape();
    h.begins[0]!.resolve(h.answer());
    await settle();
    expect(h.sent[1]).toEqual({ method: "gesture.cancel", params: { gesture: "g1" } });
    expect(h.control.get().state).toBe("idle");
  });

  it("beginning: a refusal goes idle, snaps back and names the code", async () => {
    const h = harness();
    h.control.press();
    h.control.move(-1);
    h.begins[0]!.reject(refusal("parameter-held", "parameter-held amp gainDb", { gesture: "g9", key: "gainDb", silentMs: 12000 }));
    await settle();
    expect(h.control.get()).toEqual({ state: "idle", pending: false });
    expect(h.notices).toHaveLength(1);
    expect(h.notices[0]).toMatchObject({
      code: "parameter-held",
      what: "amp gainDb",
      verb: "could not be held",
      detail: "another editor holds it, silent for 12 s",
      subject: { kind: "node", node: "amp" },
    });
  });

  it("beginning: not-live with change live sends the field's node.set instead", async () => {
    const h = harness();
    h.control.press();
    h.control.move(-2);
    h.control.release();
    h.begins[0]!.reject(refusal("not-live", "not-live amp gainDb", { key: "gainDb", change: "live" }));
    await settle();
    expect(h.instead).toEqual([{ name: "amp", key: "gainDb", value: -2 }]);
    expect(h.notices).toEqual([]);
    expect(h.control.get().state).toBe("idle");
  });

  it("active: moves send at most one tick per animation frame, the newest, seq counting up", async () => {
    const h = await active();
    h.control.move(-5);
    h.control.move(-4.5);
    expect(h.methods()).toEqual(["gesture.begin"]);
    h.frame();
    h.control.move(-4);
    h.frame();
    h.frame();
    expect(h.sent.slice(1)).toEqual([
      { method: "gesture.change", params: { gesture: "g1", seq: 1, value: -4.5 } },
      { method: "gesture.change", params: { gesture: "g1", seq: 2, value: -4 } },
    ]);
  });

  it("active: 10 s with no move sends the last tick again", async () => {
    const h = await active();
    h.control.move(-5);
    h.frame();
    h.advance(keepAliveMs - 1);
    expect(h.sent).toHaveLength(2);
    h.advance(1);
    expect(h.sent[2]).toEqual({ method: "gesture.change", params: { gesture: "g1", seq: 2, value: -5 } });
    h.advance(keepAliveMs);
    expect(h.sent[3]).toEqual({ method: "gesture.change", params: { gesture: "g1", seq: 3, value: -5 } });
  });

  it("active: a still hand that never moved keeps ear hearing it with the begin's value", async () => {
    const h = await active();
    h.advance(keepAliveMs);
    expect(h.sent[1]).toEqual({ method: "gesture.change", params: { gesture: "g1", seq: 1, value: -6 } });
  });

  it("active: a release ends at the shown value; the answer goes idle", async () => {
    const h = await active();
    h.control.move(-3.5);
    h.control.release();
    expect(h.sent[1]).toEqual({ method: "gesture.end", params: { gesture: "g1", value: -3.5 } });
    expect(h.control.get().state).toBe("ending");
    // The frame it had asked for sends nothing now.
    h.frame();
    expect(h.sent).toHaveLength(2);
    h.ends[0]!.resolve({ value: -3.5 });
    await settle();
    expect(h.control.get()).toEqual({ state: "idle", pending: false });
    expect(h.controls.holding("g1")).toBeUndefined();
  });

  it("active: a release with no move ends with no value (ear keeps what it holds)", async () => {
    const h = await active();
    h.control.release();
    expect(h.sent[1]).toEqual({ method: "gesture.end", params: { gesture: "g1" } });
  });

  it("active: Escape cancels at once", async () => {
    const h = await active();
    h.control.move(-1);
    h.control.escape();
    expect(h.sent[1]).toEqual({ method: "gesture.cancel", params: { gesture: "g1" } });
    expect(h.control.get().state).toBe("idle");
    // ear's aborted cancelled for its own cancel finds no control holding g1.
    h.changed({ gesture: "g1", phase: "aborted", reason: "cancelled", value: -6 });
    expect(h.notices).toEqual([]);
  });

  it("window blur and a hidden page let go of every held control: its end at the shown value", async () => {
    const h = await active();
    h.control.move(-2);
    h.controls.releaseAll();
    expect(h.sent[1]).toEqual({ method: "gesture.end", params: { gesture: "g1", value: -2 } });
  });

  it("a pointer press during a keyboard gesture continues it, and its release ends it", async () => {
    const h = harness();
    h.control.nudge(-5);
    h.begins[0]!.resolve(h.answer());
    await settle();
    expect(h.sent[1]).toMatchObject({ method: "gesture.change", params: { seq: 1, value: -5 } });
    h.control.press();
    expect(h.methods()).toEqual(["gesture.begin", "gesture.change"]);
    // The keys' 400 ms end does not end it while the pointer holds.
    h.advance(keyEndMs * 2);
    expect(h.control.get().state).toBe("active");
    h.control.move(-4);
    h.frame();
    expect(h.sent[2]).toMatchObject({ method: "gesture.change", params: { gesture: "g1", seq: 2, value: -4 } });
    h.control.release();
    expect(h.sent[3]).toEqual({ method: "gesture.end", params: { gesture: "g1", value: -4 } });
  });

  it("a key during a pointer gesture continues it too", async () => {
    const h = await active();
    h.control.nudge(-5.5);
    h.advance(keyEndMs);
    expect(h.control.get().state).toBe("active");
    h.control.release();
    expect(h.sent.at(-1)).toEqual({ method: "gesture.end", params: { gesture: "g1", value: -5.5 } });
  });

  it("the hidden range input's own keys are a gesture that ends 400 ms after the last", async () => {
    const h = harness();
    let shown = -6;
    const key = (name: string, shift = false) => {
      const action = rangeKey(gainDb, shown, name, shift, h.control.get().state !== "idle");
      if (action !== undefined && "nudge" in action) {
        h.control.nudge(action.nudge);
        shown = action.nudge;
      }
      return action;
    };
    expect(key("ArrowUp")).toBeDefined();
    h.begins[0]!.resolve(h.answer());
    await settle();
    key("ArrowUp");
    h.frame();
    h.advance(keyEndMs - 1);
    expect(h.control.get().state).toBe("active");
    key("End");
    h.frame();
    h.advance(keyEndMs);
    expect(h.methods()).toEqual(["gesture.begin", "gesture.change", "gesture.change", "gesture.change", "gesture.end"]);
    expect(h.sent.at(-1)).toEqual({ method: "gesture.end", params: { gesture: "g1", value: 24 } });
    // Backspace and m are not the control's, and inside .nokey not a shortcut either: they send nothing.
    expect(rangeKey(gainDb, shown, "Backspace", false, false)).toBeUndefined();
    expect(rangeKey(gainDb, shown, "m", false, false)).toBeUndefined();
    const knobInput = { tagName: "INPUT", type: "range", closest: (selector: string) => (selector === ".nokey" ? {} : null) };
    expect(isTextTarget(knobInput)).toBe(true);
    const m = { key: "m", metaKey: false, ctrlKey: false, altKey: false, shiftKey: false };
    expect(shortcutOf(m, false, isTextTarget(knobInput))).toBeUndefined();
    expect(h.methods()).toHaveLength(5);
  });

  it("a screen reader's change of the hidden input is a gesture as a key's is", async () => {
    const h = harness();
    const at = inputValueOf(gainDb, -6);
    h.control.nudge(valueOfInput(gainDb, String(at + 10)));
    h.begins[0]!.resolve(h.answer());
    await settle();
    const tick = h.sent[1]!;
    expect(tick.method).toBe("gesture.change");
    expect(tick.params.value as number).toBeGreaterThan(-6);
    h.advance(keyEndMs);
    expect(h.methods().at(-1)).toBe("gesture.end");
  });

  it("double-click sets the default as one gesture", async () => {
    const h = harness();
    h.control.setTo(0);
    h.begins[0]!.resolve(h.answer());
    await settle();
    expect(h.sent[1]).toEqual({ method: "gesture.end", params: { gesture: "g1", value: 0 } });
  });

  it("double-click while the first click's end is on its way sets the default after it", async () => {
    const h = await active();
    h.control.release();
    h.control.setTo(0);
    expect(h.methods()).toEqual(["gesture.begin", "gesture.end"]);
    h.ends[0]!.resolve({ value: -6 });
    await settle();
    expect(h.methods()).toEqual(["gesture.begin", "gesture.end", "gesture.begin"]);
  });

  it("ending: a refusal goes idle with ear's committed value and a notice", async () => {
    const h = await active();
    h.control.move(30);
    h.control.release();
    // ear tells the abort (refused) before the reply; the reply's notice says why.
    h.changed({ gesture: "g1", phase: "aborted", reason: "refused", value: -6 });
    expect(h.notices).toEqual([]);
    h.ends[0]!.reject(refusal("invalid-settings", "invalid-settings amp: gainDb: must be a number of dB from -90 to 24"));
    await settle();
    expect(h.control.get().state).toBe("idle");
    expect(h.notices).toHaveLength(1);
    expect(h.notices[0]).toMatchObject({ code: "refused", severity: "error", what: "amp gainDb" });
    expect(h.notices[0]!.detail).toContain("it stays -6.0 dB");
  });

  it("any: aborted for its id waits for the release, then idle, with a notice and Set again", async () => {
    const h = await active();
    h.control.move(-3.5);
    h.frame();
    h.changed({ gesture: "g1", phase: "aborted", reason: "hand-edit" });
    expect(h.control.get().state).toBe("aborted");
    // The words need ear's new value: they wait for the node's node.updated.
    expect(h.notices).toEqual([]);
    h.setGraph((node) => ({ ...node, settings: { gainDb: -10 } }));
    h.controls.apply({ jsonrpc: "2.0", method: "node.updated", params: { name: "amp" } } as never);
    expect(h.notices).toHaveLength(1);
    expect(h.notices[0]).toMatchObject({
      code: "hand-edit",
      detail: "ear.conf was edited by hand and changed amp gainDb while you held it; it now says -10.0 dB.",
      again: { label: "Set -3.5 dB again", name: "amp", settings: { gainDb: -3.5 } },
    });
    h.control.move(-1);
    h.frame();
    expect(h.methods().filter((method) => method === "gesture.change")).toHaveLength(1);
    h.control.release();
    expect(h.control.get().state).toBe("idle");
    expect(h.methods().at(-1)).toBe("gesture.change");
  });

  it("any: a notice waiting for ear's value is told after 1 s at the latest", async () => {
    const h = await active();
    h.changed({ gesture: "g1", phase: "aborted", reason: "file-saved" });
    h.control.release();
    expect(h.notices).toEqual([]);
    h.advance(noticeWaitMs);
    expect(h.notices[0]).toMatchObject({ code: "file-saved" });
    expect(h.notices[0]!.detail).toContain("was saved from the text pane");
  });

  it("any: key-removed says the count row's new value", async () => {
    const h = harness();
    const trim = h.controls.control("amp", "trimDb.3", { ...gainDb, min: -24, max: 24, taper: "linear" as const });
    trim.press();
    h.begins[0]!.resolve(h.answer(0));
    await settle();
    h.changed({ gesture: "g1", phase: "aborted", reason: "key-removed", value: 0 });
    h.setGraph((node) => ({ ...node, settings: { channels: 2 } }));
    h.controls.apply({ jsonrpc: "2.0", method: "node.updated", params: { name: "amp" } } as never);
    expect(h.notices[0]!.detail).toBe("amp's channels is now 2, so trimDb.3, which you held, is not used.");
  });

  it("any: end with a reason (stale) goes idle with ear's kept value", async () => {
    const h = await active();
    h.changed({ gesture: "g1", phase: "end", value: -3.5, reason: "stale" });
    expect(h.control.get().state).toBe("idle");
    expect(h.notices[0]).toMatchObject({
      code: "stale",
      detail: "ear heard nothing about amp gainDb for 30 s and kept -3.5 dB.",
    });
  });

  it("any: another gesture's id changes nothing", async () => {
    const h = await active();
    h.changed({ gesture: "g2", phase: "aborted", reason: "hand-edit" });
    expect(h.control.get().state).toBe("active");
  });

  it("a lost connection freezes it with its id and shown value; the reconnect ends it by that id", async () => {
    const h = await active();
    h.control.move(-3.5);
    h.frame();
    h.controls.connection("closed");
    expect(h.control.get()).toMatchObject({ state: "frozen", id: "g1", value: -3.5 });
    expect(h.control.get().lost).toBe(
      "Connection lost while you held amp gainDb; ear keeps the last value it heard.",
    );
    // A move or release while frozen sends nothing.
    h.control.move(0);
    h.control.release();
    h.frame();
    h.advance(keepAliveMs);
    expect(h.sent).toHaveLength(2);
    h.controls.connection("connecting");
    h.controls.connection("open");
    expect(h.sent[2]).toEqual({ method: "gesture.end", params: { gesture: "g1", value: -3.5 } });
    expect(h.control.get().state).toBe("ending");
    h.ends[0]!.resolve({ value: -3.5 });
    await settle();
    expect(h.control.get()).toEqual({ state: "idle", pending: false });
  });

  it("a connection lost while ending freezes it too, and the end is sent again", async () => {
    const h = await active();
    h.control.move(-2);
    h.control.release();
    h.controls.connection("closed");
    h.ends[0]!.reject(new ConnectionClosedError());
    await settle();
    expect(h.control.get().state).toBe("frozen");
    h.controls.connection("open");
    expect(h.sent.at(-1)).toEqual({ method: "gesture.end", params: { gesture: "g1", value: -2 } });
  });

  it("the reconnect's end finding the gesture already let go says ear kept its value", async () => {
    const h = await active();
    h.controls.connection("closed");
    h.setGraph((node) => ({ ...node, settings: { gainDb: -6 } }));
    h.controls.connection("open");
    h.ends[0]!.reject(refusal("unknown-gesture", "unknown-gesture g1"));
    await settle();
    expect(h.control.get().state).toBe("idle");
    expect(h.notices[0]).toMatchObject({ code: "unknown-gesture", severity: "info" });
    expect(h.notices[0]!.detail).toBe("ear had already let go of it, keeping -6.0 dB.");
  });

  it("a connection lost during a begin goes idle; a late answer is cancelled", async () => {
    const h = harness();
    h.control.press();
    h.controls.connection("closed");
    expect(h.control.get().state).toBe("idle");
    h.begins[0]!.reject(new ConnectionClosedError());
    await settle();
    expect(h.notices).toEqual([]);
    const late = harness();
    late.control.press();
    late.controls.connection("closed");
    late.begins[0]!.resolve(late.answer());
    await settle();
    expect(late.methods()).toEqual(["gesture.begin", "gesture.cancel"]);
  });
});
