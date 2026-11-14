// companions.ts against docs/editor.md#companions: each kind's word, an unknown kind by its own text, readiness in
// words, the details with their totals, no strip for a failed or disabled node and one kept by a stopped node, and the
// warning mark only while a counter rose in the last minute; then the companions and unknown-values fixtures replayed
// through the reducer into the badges drawn.
import { describe, expect, it } from "vitest";
import {
  CompanionMoves,
  companionBadges,
  companionDetail,
  companionLabel,
  recentMarkMs,
  type CompanionBadge,
} from "./companions";
import { Editor } from "./editor";
import { GraphStore, reduce, type GraphState } from "./graph";
import type { Companion, Graph, GraphNode, ServerMessage } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";

const sink = (dropped: number, ready = true): Companion => ({
  name: "rec.writer",
  kind: "sink",
  bank: "writer",
  ready,
  dropped,
});

function node(companions: Companion[] | undefined, health: Partial<GraphNode> = {}): GraphNode {
  return {
    name: "rec",
    type: "file",
    settings: {},
    inputs: [],
    outputs: [],
    state: "running",
    phase: "running",
    wanted: "running",
    blockLost: false,
    ...(companions !== undefined ? { companions } : {}),
    ...health,
  };
}

function graphOf(...nodes: GraphNode[]): Graph {
  return {
    graphFrequency: 48000,
    blockSize: 256,
    realtime: false,
    scheduling: "SCHED_OTHER",
    types: [],
    nodes,
    routes: [],
  };
}

/** The moves after seeing each graph in turn, the i-th at times[i]. */
function seen(graphs: Graph[], times: number[]): CompanionMoves {
  const moves = new CompanionMoves();
  graphs.forEach((graph, i) => moves.note(graph, times[i]!));
  return moves;
}

/** What a strip draws, in short: "label dot[!]". */
function drawn(badges: CompanionBadge[]): string[] {
  return badges.map((badge) => `${badge.label} ${badge.ready ? "●" : "○"}${badge.warning ? "!" : ""}`);
}

describe("companionLabel", () => {
  it("gives each known kind its short word", () => {
    expect(["card-playback", "card-capture", "rtp-receiver", "sink", "source"].map(companionLabel)).toEqual([
      "card out",
      "card in",
      "rtp",
      "sink",
      "source",
    ]);
  });

  it("draws an unknown kind by its own text, a name Object's prototype has included", () => {
    expect(companionLabel("paced-source")).toBe("paced-source");
    expect(companionLabel("toString")).toBe("toString");
  });
});

describe("companionDetail", () => {
  it("names the companion, its word and bank, its readiness and its counter's total", () => {
    expect(companionDetail(sink(4096))).toBe("rec.writer (sink on writer): ready; 4096 frames dropped since it was made");
    expect(companionDetail(sink(1, false))).toBe(
      "rec.writer (sink on writer): not ready; 1 frame dropped since it was made",
    );
    expect(companionDetail({ name: "feed.from_feed", kind: "source", bank: "from_feed", ready: true, shortfall: 64 })).toBe(
      "feed.from_feed (source on from_feed): ready; 64 frames played short since it was made",
    );
  });

  it("gives an engine kind no counter", () => {
    expect(companionDetail({ name: "desk.playback", kind: "card-playback", bank: "to_card", ready: false })).toBe(
      "desk.playback (card out on to_card): not ready",
    );
  });

  it("shows a counter it knows on a kind it does not, and ignores a member it does not know", () => {
    const paced = { name: "n.from_feed", kind: "paced-source", bank: "from_feed", ready: true, shortfall: 0, lagFrames: 120 };
    expect(companionDetail(paced)).toBe("n.from_feed (paced-source on from_feed): ready; 0 frames played short since it was made");
  });
});

describe("companionBadges", () => {
  const none = new CompanionMoves();

  it("draws one badge per companion in list order, readiness in words", () => {
    const desk = node([
      { name: "desk.playback", kind: "card-playback", bank: "to_card", ready: true },
      { name: "desk.capture", kind: "card-capture", bank: "from_card", ready: false },
    ]);
    const badges = companionBadges(desk, none, 0);
    expect(badges.map((badge) => [badge.key, badge.label, badge.readyText])).toEqual([
      ["desk.playback", "card out", "ready"],
      ["desk.capture", "card in", "not ready"],
    ]);
    expect(badges[1]!.title).toBe("desk.capture (card in on from_card): not ready");
  });

  it("draws no strip for a node with no list or an empty one", () => {
    expect(companionBadges(node(undefined), none, 0)).toEqual([]);
    expect(companionBadges(node([]), none, 0)).toEqual([]);
  });

  it("draws no strip for a failed or disabled node, whatever list it holds; a stopped node keeps its strip", () => {
    const list = [sink(0)];
    expect(companionBadges(node(list, { state: "failed", phase: "failed" }), none, 0)).toEqual([]);
    expect(companionBadges(node(list, { state: "disabled", phase: "disabled" }), none, 0)).toEqual([]);
    expect(drawn(companionBadges(node(list, { state: "stopped", phase: "stopped" }), none, 0))).toEqual(["sink ●"]);
    expect(drawn(companionBadges(node(list, { state: "running", phase: "rebinding" }), none, 0))).toEqual(["sink ●"]);
  });
});

describe("the warning mark", () => {
  const at = 1_000_000;

  it("shows while a counter rose in the last minute, with its words in the title", () => {
    const moves = seen([graphOf(node([sink(0)])), graphOf(node([sink(4096)]))], [at - 5000, at]);
    const rising = node([sink(4096)]);
    const [badge] = companionBadges(rising, moves, at + recentMarkMs - 1);
    expect(badge!.warning).toBe(true);
    expect(badge!.warningText).toBe("dropped frames in the last minute");
    expect(badge!.title).toBe(
      "rec.writer (sink on writer): ready; 4096 frames dropped since it was made (dropped frames in the last minute)",
    );
    expect(companionBadges(rising, moves, at + recentMarkMs)[0]!.warning).toBe(false);
    expect(companionBadges(rising, moves, at + recentMarkMs)[0]!.warningText).toBe("");
  });

  it("is not shown for a counter above zero that has not risen since it was first seen", () => {
    const moves = seen([graphOf(node([sink(4096)])), graphOf(node([sink(4096)]))], [at, at + 1000]);
    expect(companionBadges(node([sink(4096)]), moves, at + 1000)[0]!.warning).toBe(false);
    expect(moves.movedAt("rec", "rec.writer")).toBeUndefined();
  });

  it("keeps the time of the last rise while the counter holds still, and takes a later rise's", () => {
    const moves = seen(
      [graphOf(node([sink(0)])), graphOf(node([sink(10)])), graphOf(node([sink(10)])), graphOf(node([sink(20)]))],
      [at, at + 1, at + 2, at + 3],
    );
    expect(moves.movedAt("rec", "rec.writer")).toBe(at + 3);
    const still = seen([graphOf(node([sink(0)])), graphOf(node([sink(10)])), graphOf(node([sink(10)]))], [at, at + 1, at + 2]);
    expect(still.movedAt("rec", "rec.writer")).toBe(at + 1);
  });

  it("takes a fall as a new configuration's first value, not a rise", () => {
    const moves = seen([graphOf(node([sink(0)])), graphOf(node([sink(10)])), graphOf(node([sink(3)]))], [at, at + 1, at + 2]);
    expect(moves.movedAt("rec", "rec.writer")).toBeUndefined();
  });

  it("forgets a companion its list dropped, so the next configuration's first value is no rise", () => {
    const moves = seen([graphOf(node([sink(10)])), graphOf(node([])), graphOf(node([sink(20)]))], [at, at + 1, at + 2]);
    expect(moves.movedAt("rec", "rec.writer")).toBeUndefined();
    const gone = seen([graphOf(node([sink(10)])), graphOf(), graphOf(node([sink(20)]))], [at, at + 1, at + 2]);
    expect(gone.movedAt("rec", "rec.writer")).toBeUndefined();
  });

  it("marks a source's shortfall, and a counter on a kind it does not know", () => {
    const source = (shortfall: number): Companion => ({ name: "rec.feed", kind: "mystery", bank: "feed", ready: true, shortfall });
    const moves = seen([graphOf(node([source(0)])), graphOf(node([source(5)]))], [at, at + 1]);
    const [badge] = companionBadges(node([source(5)]), moves, at + 1);
    expect(badge!.label).toBe("mystery");
    expect(badge!.warningText).toBe("played short in the last minute");
  });

  it("is never shown on a failed node, whose strip is empty", () => {
    const moves = seen([graphOf(node([sink(0)])), graphOf(node([sink(9)]))], [at, at + 1]);
    expect(companionBadges(node([sink(9)], { state: "failed", phase: "failed" }), moves, at + 1)).toEqual([]);
    expect(moves.nextExpiry(node([sink(9)], { state: "failed", phase: "failed" }), at + 1)).toBeUndefined();
  });

  it("says when the next shown mark leaves, the box's time to redraw", () => {
    const two = (a: number, b: number) =>
      node([sink(a), { name: "rec.feed", kind: "source", bank: "feed", ready: true, shortfall: b }]);
    // The first in the list rose last: the earliest end is the second's.
    const moves = seen([graphOf(two(0, 0)), graphOf(two(0, 1)), graphOf(two(1, 1))], [at, at + 10, at + 20]);
    expect(moves.nextExpiry(two(1, 1), at + 20)).toBe(at + 10 + recentMarkMs);
    expect(moves.nextExpiry(two(1, 1), at + 10 + recentMarkMs)).toBe(at + 20 + recentMarkMs);
    expect(moves.nextExpiry(two(1, 1), at + 20 + recentMarkMs)).toBeUndefined();
  });

  it("notes each graph its store makes, from when it attaches", () => {
    const store = new GraphStore();
    const moves = new CompanionMoves();
    let now = at;
    moves.attach(store, () => now);
    const order: string[] = [];
    store.subscribe(() => order.push(moves.movedAt("rec", "rec.writer") === undefined ? "unmoved" : "moved"));
    store.apply({ jsonrpc: "2.0", id: 1, result: graphOf(node([sink(0)])) as never });
    now = at + 7;
    store.apply({ jsonrpc: "2.0", method: "node.companionsChanged", params: { name: "rec", companions: [sink(64)] } });
    expect(order).toEqual(["unmoved", "moved"]);
    expect(moves.movedAt("rec", "rec.writer")).toBe(at + 7);
  });

  it("is noted by an editor session before anything that draws hears its graph", () => {
    const editor = new Editor({ url: "ws://ear.invalid/rpc" });
    const heard: (number | undefined)[] = [];
    editor.graph.subscribe(() => heard.push(editor.moves.movedAt("rec", "rec.writer")));
    editor.graph.apply({ jsonrpc: "2.0", id: 1, result: graphOf(node([sink(0)])) as never });
    editor.graph.apply({ jsonrpc: "2.0", method: "node.companionsChanged", params: { name: "rec", companions: [sink(64)] } });
    expect(heard[0]).toBeUndefined();
    expect(heard[1]).toBeTypeOf("number");
  });
});

describe("the fixtures, replayed through the reducer into the badges drawn", () => {
  /** Each node's strip after each message, the moves fed one message a second. */
  function replay(messages: ServerMessage[]): { states: GraphState[]; moves: CompanionMoves; strips: Record<string, string[]>[] } {
    const moves = new CompanionMoves();
    const states: GraphState[] = [];
    const strips: Record<string, string[]>[] = [];
    let state: GraphState = null;
    messages.forEach((message, i) => {
      state = reduce(state, message);
      moves.note(state, i * 1000);
      states.push(state);
      strips.push(
        Object.fromEntries((state?.nodes ?? []).map((n) => [n.name, drawn(companionBadges(n, moves, i * 1000))])),
      );
    });
    return { states, moves, strips };
  }

  const companions = messagesOf(fixtures.companions.messages);
  const { strips } = replay(companions);
  const index = (predicate: (message: ServerMessage) => boolean, from = 0) =>
    companions.findIndex((message, i) => i >= from && predicate(message));
  const paramsOf = (message: ServerMessage, method: string) =>
    "method" in message && message.method === method ? (message.params as Record<string, unknown>) : undefined;
  const isList = (name: string) => (message: ServerMessage) => paramsOf(message, "node.companionsChanged")?.name === name;
  const isState = (name: string, phase: string) => (message: ServerMessage) => {
    const params = paramsOf(message, "node.state");
    return params?.name === name && params.phase === phase;
  };

  it("draws the snapshot's every kind, no strip for white, no mark for a total first seen", () => {
    expect(strips[0]).toEqual({
      desk: ["card out ●", "card in ●"],
      fpp: ["rtp ●"],
      noise: [],
      rec: ["sink ●"],
    });
  });

  it("draws a new node's sink hollow while it configures, then filled before its configured", () => {
    const added = index((m) => "method" in m && m.method === "node.added");
    expect(strips[added]!.take).toEqual([]);
    const notReady = index(isList("take"));
    expect(strips[notReady]!.take).toEqual(["sink ○"]);
    const ready = index(isList("take"), notReady + 1);
    expect(strips[ready]!.take).toEqual(["sink ●"]);
    expect(index(isState("take", "configured"))).toBeGreaterThan(ready);
  });

  it("marks rec's sink when its dropped rises, and keeps the strip across its stop", () => {
    const rose = index(isList("rec"));
    expect(strips[rose]!.rec).toEqual(["sink ●!"]);
    const stopped = index(isState("rec", "stopped"));
    expect(strips[stopped]!.rec).toEqual(["sink ●!"]);
  });

  it("empties rec's strip for its restart, then draws the next configuration's sink unmarked", () => {
    const emptied = index(
      (m) => isList("rec")(m) && (paramsOf(m, "node.companionsChanged")!.companions as unknown[]).length === 0,
    );
    expect(strips[emptied]!.rec).toEqual([]);
    const next = index(isList("rec"), emptied + 1);
    expect(strips[next]!.rec).toEqual(["sink ○"]);
    expect(strips[strips.length - 1]!.rec).toEqual(["sink ●"]);
  });

  it("draws no strip for fpp once failed, before its empty list arrives", () => {
    const failed = index(isState("fpp", "failed"));
    expect(strips[failed - 1]!.fpp).toEqual(["rtp ○"]);
    expect(strips[failed]!.fpp).toEqual([]);
  });

  it("draws nothing for a removed node, and the fixture's final graph as its strips", () => {
    const final = strips[strips.length - 1]!;
    expect(final).toEqual({ desk: ["card out ●", "card in ●"], fpp: [], noise: [], rec: ["sink ●"] });
    const moves = new CompanionMoves();
    moves.note(fixtures.companions.graph, 0);
    expect(Object.fromEntries(fixtures.companions.graph.nodes.map((n) => [n.name, drawn(companionBadges(n, moves, 0))]))).toEqual(final);
  });

  it("draws the unknown-values fixture's unknown kind by its text, its known counter in words", () => {
    const { states, moves } = replay(messagesOf(fixtures.unknownValues.messages));
    const last = states[states.length - 1]!;
    const noise = last.nodes.find((n) => n.name === "noise")!;
    const [badge] = companionBadges(noise, moves, 0);
    expect(badge!.label).toBe("paced-source");
    expect(badge!.readyText).toBe("ready");
    expect(badge!.title).toBe("noise.from_feed (paced-source on from_feed): ready; 0 frames played short since it was made");
  });
});
