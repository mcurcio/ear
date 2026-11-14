// counters.ts against docs/editor.md#counters: every total in the panel in words, an unknown counter by its name and
// number; the node's mark and the graph's dropouts badge only while a loss counter rose in the last minute (never
// `cycles`, never an unknown counter), by the companions' rule (a first sighting, a fall, a reconnect's snapshot are
// no rise) on the monotonic clock; then the counters-changed and unknown-values fixtures replayed through the reducer
// into what is drawn.
import { describe, expect, it, vi } from "vitest";
import {
  CounterMoves,
  dropoutBadge,
  graphCounterRows,
  nodeCounterMark,
  nodeCounterRows,
  type CounterRow,
} from "./counters";
import { Editor } from "./editor";
import { GraphStore, reduce, type GraphState } from "./graph";
import { recentMarkMs } from "./marks";
import type { Graph, GraphCounters, GraphNode, NodeCounters, ServerMessage } from "./protocol";
import type { SocketEvents } from "./rpc";
import { fixtures, messagesOf } from "./testFixtures";

const nodeCounts = (counts: Partial<NodeCounters> & Record<string, number> = {}): NodeCounters => ({
  missedCycles: 0,
  overruns: 0,
  repositions: 0,
  driverChanges: 0,
  ...counts,
});

const graphCounts = (counts: Partial<GraphCounters> & Record<string, number> = {}): GraphCounters => ({
  cycles: 0,
  cutCycles: 0,
  stalls: 0,
  lostMs: 0,
  ...counts,
});

function node(counters: NodeCounters | undefined, name = "rec"): GraphNode {
  return {
    name,
    type: "file",
    settings: {},
    inputs: [],
    outputs: [],
    state: "running",
    phase: "running",
    wanted: "running",
    blockLost: false,
    ...(counters !== undefined ? { counters } : {}),
  };
}

function graphOf(counters: GraphCounters | undefined, ...nodes: GraphNode[]): Graph {
  return {
    graphFrequency: 48000,
    blockSize: 256,
    realtime: true,
    scheduling: "SCHED_FIFO 88",
    types: [],
    nodes,
    routes: [],
    ...(counters !== undefined ? { counters } : {}),
  };
}

/** The moves after seeing each graph in turn, the i-th at times[i]. */
function seen(graphs: Graph[], times: number[]): CounterMoves {
  const moves = new CounterMoves();
  graphs.forEach((graph, i) => moves.note(graph, times[i]!));
  return moves;
}

/** A table in short: "label text[!]". */
function table(rows: CounterRow[]): string[] {
  return rows.map((row) => `${row.label} ${row.text}${row.warning ? "!" : ""}`);
}

const at = 2_000_000;

describe("the panel's totals", () => {
  const none = new CounterMoves();

  it("shows the graph's counters in words, lostMs in ms, then an unknown one by its name and number", () => {
    const graph = graphOf(graphCounts({ cycles: 18750, cutCycles: 39, stalls: 1, lostMs: 208, lateWakes: 2 }));
    const rows = graphCounterRows(graph, none, at);
    expect(table(rows)).toEqual(["cycles 18750", "dropouts 39", "stalls 1", "time lost 208 ms", "lateWakes 2"]);
    expect(rows.map((row) => row.known)).toEqual([true, true, true, true, false]);
    expect(rows[1]!.title).toBe("cycles the driver cut short: a dropout on every output");
    expect(rows[4]!.title).toBe("a counter this editor does not know");
  });

  it("shows a node's counters in words, an unknown one last, zeros included", () => {
    const rows = nodeCounterRows(node(nodeCounts({ missedCycles: 33, overruns: 2, underruns: 1 })), none, at);
    expect(table(rows)).toEqual([
      "missed cycles 33",
      "overruns 2",
      "repositions 0",
      "driver changes 0",
      "underruns 1",
    ]);
  });

  it("shows an unknown counter named like a known one of the other scope, or like Object's prototype, by its name", () => {
    const rows = nodeCounterRows(node(nodeCounts({ cycles: 4, toString: 1 })), none, at);
    expect(table(rows).slice(4)).toEqual(["cycles 4", "toString 1"]);
  });

  it("shows nothing without counters: an ear without the feature", () => {
    expect(graphCounterRows(graphOf(undefined), none, at)).toEqual([]);
    expect(nodeCounterRows(node(undefined), none, at)).toEqual([]);
    expect(nodeCounterMark(node(undefined), none, at).warning).toBe(false);
    expect(dropoutBadge(graphOf(undefined), none, at)).toBeUndefined();
  });
});

describe("the node's mark", () => {
  const rec = (counts: Partial<NodeCounters> & Record<string, number>) => graphOf(graphCounts(), node(nodeCounts(counts)));

  it("shows while missedCycles rose in the last minute, the last minute's and the totals in its words", () => {
    const moves = seen([rec({}), rec({ missedCycles: 12, overruns: 3 })], [at - 1000, at]);
    const now = node(nodeCounts({ missedCycles: 12, overruns: 3 }));
    const mark = nodeCounterMark(now, moves, at + recentMarkMs - 1);
    expect(mark.warning).toBe(true);
    expect(mark.warningText).toBe("in the last minute: missed 12 cycles, overran 3 times");
    expect(mark.title).toBe(
      "In the last minute: missed 12 cycles, overran 3 times. Since it was added: missed 12 cycles, overran 3 times.",
    );
    expect(nodeCounterMark(now, moves, at + recentMarkMs).warning).toBe(false);
    expect(nodeCounterMark(now, moves, at + recentMarkMs).title).toBe("");
  });

  it("shows for each of the node's known counters", () => {
    for (const key of ["missedCycles", "overruns", "repositions", "driverChanges"]) {
      const moves = seen([rec({}), rec({ [key]: 1 })], [at, at + 1]);
      expect(nodeCounterMark(node(nodeCounts({ [key]: 1 })), moves, at + 1).warning, key).toBe(true);
    }
    const moves = seen([rec({}), rec({ overruns: 1 })], [at, at + 1]);
    expect(nodeCounterMark(node(nodeCounts({ overruns: 1 })), moves, at + 1).warningText).toBe(
      "in the last minute: overran 1 time",
    );
  });

  it("says in the totals what rose before the last minute too, the last minute's only what rose in it", () => {
    const moves = seen([rec({ missedCycles: 40 }), rec({ missedCycles: 40, overruns: 1 })], [at, at + 1]);
    const mark = nodeCounterMark(node(nodeCounts({ missedCycles: 40, overruns: 1 })), moves, at + 1);
    expect(mark.title).toBe("In the last minute: overran 1 time. Since it was added: missed 40 cycles, overran 1 time.");
  });

  it("is not shown for an unknown counter's rise, nor for totals first seen above zero", () => {
    const moves = seen([rec({ missedCycles: 7, underruns: 0 }), rec({ missedCycles: 7, underruns: 5 })], [at, at + 1]);
    expect(nodeCounterMark(node(nodeCounts({ missedCycles: 7, underruns: 5 })), moves, at + 1).warning).toBe(false);
    const rows = nodeCounterRows(node(nodeCounts({ missedCycles: 7, underruns: 5 })), moves, at + 1);
    expect(table(rows)).toEqual(["missed cycles 7", "overruns 0", "repositions 0", "driver changes 0", "underruns 5"]);
  });

  it("is not shown for a fall (a node added again under the name), nor for the graph's losses", () => {
    const fell = seen([rec({ missedCycles: 7 }), rec({ missedCycles: 3 })], [at, at + 1]);
    expect(nodeCounterMark(node(nodeCounts({ missedCycles: 3 })), fell, at + 1).warning).toBe(false);
    fell.note(rec({ missedCycles: 5 }), at + 2);
    expect(nodeCounterRows(node(nodeCounts({ missedCycles: 5 })), fell, at + 2)[0]!.warningText).toBe(
      "2 more in the last minute",
    );
    const host = seen(
      [graphOf(graphCounts(), node(nodeCounts())), graphOf(graphCounts({ cutCycles: 9, stalls: 1 }), node(nodeCounts()))],
      [at, at + 1],
    );
    expect(nodeCounterMark(node(nodeCounts()), host, at + 1).warning).toBe(false);
  });

  it("marks the rows that rose with how much in the last minute, and says when the next count leaves", () => {
    const moves = seen([rec({}), rec({ missedCycles: 2 }), rec({ missedCycles: 5 })], [at, at + 10, at + 20]);
    const rows = nodeCounterRows(node(nodeCounts({ missedCycles: 5 })), moves, at + 20);
    expect(table(rows)[0]).toBe("missed cycles 5!");
    expect(rows[0]!.warningText).toBe("5 more in the last minute");
    expect(moves.nextNodeChange(node(nodeCounts({ missedCycles: 5 })), at + 20)).toBe(at + 10 + recentMarkMs);
    expect(nodeCounterRows(node(nodeCounts({ missedCycles: 5 })), moves, at + 10 + recentMarkMs)[0]!.warningText).toBe(
      "3 more in the last minute",
    );
    expect(moves.nextNodeChange(node(nodeCounts({ missedCycles: 5 })), at + 20 + recentMarkMs)).toBeUndefined();
  });
});

describe("the dropouts badge", () => {
  const g = (counts: Record<string, number>) => graphOf(graphCounts(counts));

  it("shows while cutCycles rose in the last minute, with the minute's count and the totals in words", () => {
    const moves = seen([g({ cutCycles: 39, stalls: 1, lostMs: 208 }), g({ cutCycles: 41, stalls: 2, lostMs: 218 })], [at, at + 1]);
    const badge = dropoutBadge(g({ cutCycles: 41, stalls: 2, lostMs: 218 }), moves, at + 1)!;
    expect(badge.count).toBe(2);
    expect(badge.text).toBe("2 dropouts");
    expect(badge.title).toBe(
      "The host held up the audio thread: 2 dropouts in the last minute; 41 dropouts (218 ms) in 2 stalls since ear started.",
    );
    expect(dropoutBadge(g({ cutCycles: 41 }), moves, at + 1 + recentMarkMs)).toBeUndefined();
  });

  it("counts the rises of the last minute only, each leaving at its own minute's end", () => {
    const moves = seen([g({ cutCycles: 0 }), g({ cutCycles: 1 }), g({ cutCycles: 4 })], [at, at + 10, at + 20]);
    expect(dropoutBadge(g({ cutCycles: 4 }), moves, at + 20)!.text).toBe("4 dropouts");
    expect(moves.nextGraphChange(at + 20)).toBe(at + 10 + recentMarkMs);
    expect(dropoutBadge(g({ cutCycles: 4 }), moves, at + 10 + recentMarkMs)!.text).toBe("3 dropouts");
    expect(dropoutBadge(g({ cutCycles: 1 }), seen([g({ cutCycles: 0 }), g({ cutCycles: 1 })], [at, at]), at)!.text).toBe(
      "1 dropout",
    );
  });

  it("is never shown for cycles moving, which they do every cycle, nor an unknown graph counter", () => {
    const moves = seen(
      [g({ cycles: 100, lateWakes: 0 }), g({ cycles: 200, lateWakes: 3 })],
      [at, at + 1],
    );
    expect(dropoutBadge(g({ cycles: 200 }), moves, at + 1)).toBeUndefined();
    expect(table(graphCounterRows(g({ cycles: 200, lateWakes: 3 }), moves, at + 1))).toEqual([
      "cycles 200",
      "dropouts 0",
      "stalls 0",
      "time lost 0 ms",
      "lateWakes 3",
    ]);
  });

  it("is not shown for dropouts a first snapshot already counted", () => {
    const moves = seen([g({ cutCycles: 44 }), g({ cutCycles: 44 })], [at, at + 1]);
    expect(dropoutBadge(g({ cutCycles: 44 }), moves, at + 1)).toBeUndefined();
  });
});

describe("an editor session's counter moves", () => {
  it("times a rise on the monotonic clock, never the wall clock", () => {
    const monotonic = vi.spyOn(performance, "now").mockReturnValue(5000);
    const wall = vi.spyOn(Date, "now").mockReturnValue(9_999_999);
    try {
      const store = new GraphStore();
      const moves = new CounterMoves();
      moves.attach(store);
      store.apply({ jsonrpc: "2.0", id: 1, result: graphOf(graphCounts(), node(nodeCounts())) as never });
      store.apply({
        jsonrpc: "2.0",
        method: "counters.changed",
        params: { graph: graphCounts({ cutCycles: 1 }), nodes: { rec: nodeCounts({ overruns: 1 }) } },
      });
      expect(moves.movedAt("graph", "cutCycles")).toBe(5000);
      expect(nodeCounterMark(store.get()!.nodes[0]!, moves, 5000).warning).toBe(true);
      expect(nodeCounterMark(store.get()!.nodes[0]!, moves, 5000 + recentMarkMs).warning).toBe(false);
    } finally {
      monotonic.mockRestore();
      wall.mockRestore();
    }
  });

  it("takes a reconnect's snapshot as a first sighting: losses while away are in the totals, never marked", () => {
    const sockets: SocketEvents[] = [];
    const retries: (() => void)[] = [];
    const editor = new Editor({
      url: "ws://ear.invalid/rpc",
      openSocket: (_url, events) => {
        sockets.push(events);
        return { send: () => undefined, close: () => undefined };
      },
      schedule: (retry) => void retries.push(retry),
    });
    const snapshot = (n: number) =>
      JSON.stringify({
        jsonrpc: "2.0",
        id: 1,
        result: graphOf(graphCounts({ cutCycles: n }), node(nodeCounts({ missedCycles: n }))),
      });
    editor.client.start();
    sockets[0]!.open();
    sockets[0]!.message(snapshot(0));
    sockets[0]!.close();
    retries.shift()!();
    sockets[1]!.open();
    sockets[1]!.message(snapshot(90));
    const graph = editor.graph.get()!;
    const now = performance.now();
    expect(dropoutBadge(graph, editor.counterMoves, now)).toBeUndefined();
    expect(nodeCounterMark(graph.nodes[0]!, editor.counterMoves, now).warning).toBe(false);
    expect(table(graphCounterRows(graph, editor.counterMoves, now))[1]).toBe("dropouts 90");
    editor.client.stop();
  });

  it("is noted by an editor session before anything that draws hears its graph", () => {
    const editor = new Editor({ url: "ws://ear.invalid/rpc" });
    const heard: boolean[] = [];
    editor.graph.subscribe(() =>
      heard.push(nodeCounterMark(editor.graph.get()!.nodes[0]!, editor.counterMoves, performance.now()).warning),
    );
    editor.graph.apply({ jsonrpc: "2.0", id: 1, result: graphOf(graphCounts(), node(nodeCounts())) as never });
    editor.graph.apply({
      jsonrpc: "2.0",
      method: "counters.changed",
      params: { graph: graphCounts(), nodes: { rec: nodeCounts({ missedCycles: 3 }) } },
    });
    expect(heard).toEqual([false, true]);
  });
});

describe("the fixtures, replayed through the reducer into what is drawn", () => {
  interface Drawn {
    badge: string | undefined;
    graph: string[];
    marks: Record<string, boolean>;
    tables: Record<string, string[]>;
  }

  /** What is drawn after each message, the moves fed one message a second. */
  function replay(messages: ServerMessage[]): { drawn: Drawn[]; states: GraphState[] } {
    const moves = new CounterMoves();
    const drawn: Drawn[] = [];
    const states: GraphState[] = [];
    let state: GraphState = null;
    messages.forEach((message, i) => {
      const now = i * 1000;
      state = reduce(state, message);
      moves.note(state, now);
      states.push(state);
      const nodes = state?.nodes ?? [];
      drawn.push({
        badge: state === null ? undefined : dropoutBadge(state, moves, now)?.text,
        graph: state === null ? [] : table(graphCounterRows(state, moves, now)),
        marks: Object.fromEntries(nodes.map((n) => [n.name, nodeCounterMark(n, moves, now).warning])),
        tables: Object.fromEntries(nodes.map((n) => [n.name, table(nodeCounterRows(n, moves, now))])),
      });
    });
    return { drawn, states };
  }

  const messages = messagesOf(fixtures.countersChanged.messages);
  const { drawn } = replay(messages);
  const isMethod = (method: string, from = 0) =>
    messages.findIndex((message, i) => i >= from && "method" in message && message.method === method);

  it("draws the snapshot's totals unmarked: losses before the editor looked are not the last minute's", () => {
    expect(drawn[0]).toEqual({
      badge: undefined,
      graph: ["cycles 18750", "dropouts 39", "stalls 1", "time lost 208 ms"],
      marks: { desk: false, rec: false },
      tables: {
        desk: ["missed cycles 0", "overruns 0", "repositions 0", "driver changes 0"],
        rec: ["missed cycles 33", "overruns 2", "repositions 0", "driver changes 0"],
      },
    });
  });

  it("marks rec and shows the badge at the first sample, desk at the second; the ghost changes nothing", () => {
    const first = isMethod("counters.changed");
    expect(drawn[first]!.badge).toBe("2 dropouts");
    expect(drawn[first]!.marks).toEqual({ desk: false, rec: true });
    expect(drawn[first]!.tables.rec![1]).toBe("overruns 3!");
    expect(drawn[first]!.graph).toEqual(["cycles 37500", "dropouts 41!", "stalls 2!", "time lost 218 ms"]);
    const second = isMethod("counters.changed", first + 1);
    expect(drawn[second]!.badge).toBe("5 dropouts");
    expect(drawn[second]!.marks).toEqual({ desk: true, rec: true });
    expect(drawn[second]!.tables.desk![0]).toBe("missed cycles 12!");
    expect(Object.keys(drawn[second]!.tables)).toEqual(["desk", "rec"]);
  });

  it("takes the port-caused replace's carried counts as a first sighting, then marks the next rise", () => {
    const added = isMethod("node.added");
    expect(drawn[added]!.marks.desk).toBe(false);
    expect(drawn[added]!.tables.desk![0]).toBe("missed cycles 12");
    const last = drawn[drawn.length - 1]!;
    expect(last.marks).toEqual({ desk: true, rec: true });
    expect(last.tables.desk).toEqual(["missed cycles 12", "overruns 1!", "repositions 0", "driver changes 0"]);
    expect(last.badge).toBe("5 dropouts");
  });

  it("draws the fixture's final graph as its tables", () => {
    const final = fixtures.countersChanged.graph;
    const moves = new CounterMoves();
    moves.note(final, 0);
    const last = drawn[drawn.length - 1]!;
    const unmark = (rows: string[]) => rows.map((row) => row.replace(/!$/, ""));
    expect(table(graphCounterRows(final, moves, 0))).toEqual(unmark(last.graph));
    for (const n of final.nodes) expect(table(nodeCounterRows(n, moves, 0))).toEqual(unmark(last.tables[n.name]!));
  });

  it("draws the unknown-values fixture's unknown counters by name and number, and marks nothing for them", () => {
    const { drawn: unknown } = replay(messagesOf(fixtures.unknownValues.messages));
    const last = unknown[unknown.length - 1]!;
    expect(last.graph).toEqual(["cycles 18750", "dropouts 0", "stalls 0", "time lost 0 ms", "lateWakes 2"]);
    expect(last.tables.noise).toEqual(["missed cycles 0", "overruns 0", "repositions 0", "driver changes 0", "underruns 1"]);
    expect(last.badge).toBeUndefined();
    expect(Object.values(last.marks).every((mark) => !mark)).toBe(true);
  });
});
