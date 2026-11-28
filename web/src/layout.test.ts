// First placement: unplaced nodes get a dagre position, sources left of their sinks; a node the
// user placed keeps its position.
import { describe, expect, it } from "vitest";
import {
  NODE_FOOTER,
  NODE_HEADER,
  NODE_LINE,
  NODE_PAD,
  NODE_ROW,
  NODE_WIDTH,
  nodeBoxSize,
} from "@mcurcio/patchbay/graph";
import { reduce, type GraphState } from "./graph";
import { Places, nodePorts, nodeSize, placeNodes, quickPlace, type PlaceKeeper, type Position } from "./layout";
import type { Graph, GraphNode } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";

const graph = fixtures.snapshot.graph;

// The box is patchbay's NodeBox, so its size is patchbay's exported geometry (docs/editor.md#the-node-box).
describe("a box's size", () => {
  const running = graph.nodes.find((node) => node.state === "running" && (node.companions ?? []).length === 0)!;
  const node = (inputs: number, outputs: number, over: Partial<GraphNode> = {}): GraphNode => ({
    ...running,
    inputs: Array.from({ length: inputs }, (_, i) => `in.${i + 1}`),
    outputs: Array.from({ length: outputs }, (_, i) => `out.${i + 1}`),
    ...over,
  });

  it("is NodeBox's, its ports stacked: one row per port, the inputs' and then the outputs'", () => {
    expect(nodePorts).toBe("stacked");
    expect(nodeSize(node(4, 1))).toStrictEqual({ width: NODE_WIDTH, height: NODE_HEADER + 2 * NODE_PAD + 5 * NODE_ROW });
    expect(nodeSize(node(2, 3))).toStrictEqual(
      nodeBoxSize({ inputs: 2, outputs: 3, ports: "stacked", line: false, footer: false }),
    );
    expect(nodeSize(node(0, 0))).toStrictEqual({ width: NODE_WIDTH, height: NODE_HEADER });
  });

  it("counts the line and the companions' footer when the box draws them", () => {
    const failed = node(1, 1, { state: "failed", phase: "failed", reason: { code: "x", detail: "it broke" } });
    expect(nodeSize(failed).height).toBe(NODE_HEADER + NODE_LINE + 2 * NODE_PAD + 2 * NODE_ROW);
    const companion = { name: "rec.writer", kind: "sink", ready: true } as NonNullable<GraphNode["companions"]>[number];
    expect(nodeSize(node(1, 0, { companions: [companion] })).height).toBe(
      NODE_HEADER + 2 * NODE_PAD + NODE_ROW + NODE_FOOTER,
    );
  });
});

describe("the first layout", () => {
  it("places every node, each source left of the nodes it feeds", () => {
    const positions = placeNodes(graph, new Map());
    expect([...positions.keys()].sort()).toEqual(graph.nodes.map((node) => node.name));
    for (const route of graph.routes) {
      expect(positions.get(route.from.node)!.x + NODE_WIDTH).toBeLessThan(positions.get(route.to.node)!.x);
    }
  });

  it("keeps a placed node where it is and places only the rest", () => {
    const placed = new Map([["rec", { x: -500, y: 900 }]]);
    const positions = placeNodes(graph, placed);
    expect(positions.get("rec")).toStrictEqual({ x: -500, y: 900 });
    expect(positions.size).toBe(graph.nodes.length);
    expect(placeNodes(graph, positions)).toStrictEqual(positions);
  });
});

// A node whose ports change stays (node.updated with its new ports, feature reshape), so it keeps its place; a node
// replaced by another type (node.removed, then node.added) keeps it too, drawn between the two.
describe("places", () => {
  it("keeps a reshaped node's place, drawn after every message of the graph-file fixture", () => {
    const messages = messagesOf(fixtures.graphFile.messages);
    const places = new Places();
    let state: GraphState = null;
    let current = new Map<string, Position>();
    const dragged = { x: 1234, y: -56 };
    let deskSeenGone = false;
    for (const message of messages) {
      state = reduce(state, message);
      if (state === null) continue;
      current = places.place(state, current);
      if (current.has("desk") && current.get("desk") !== dragged && !deskSeenGone) {
        current.set("desk", dragged); // the person drags desk before the replacement
      }
      if (!state.nodes.some((node) => node.name === "desk")) deskSeenGone = true;
    }
    expect(deskSeenGone).toBe(false); // reshaped, never removed
    expect(state!.nodes.find((node) => node.name === "desk")!.inputs).toHaveLength(4);
    expect(current.get("desk")).toStrictEqual(dragged);
  });

  it("lays every node out again on tidy, forgetting a dragged place", () => {
    const places = new Places();
    const first = places.place(graph, new Map());
    const dragged = new Map(first);
    dragged.set("rec", { x: 4321, y: 99 });
    dragged.set("gone", { x: 1, y: 2 });
    places.place(graph, dragged);
    const tidied = places.relayout(graph);
    expect(tidied).toStrictEqual(placeNodes(graph, new Map()));
    expect(tidied.get("rec")).not.toStrictEqual({ x: 4321, y: 99 });
    expect(places.place(graph, new Map())).toStrictEqual(tidied);
  });
});

// Quick add's placement hints (docs/editor.md#adding-on-the-canvas) and the kept places
// (docs/editor.md#what-the-browser-keeps).
describe("placement hints", () => {
  const withNode = (base: Graph, name: string): Graph => ({
    ...base,
    nodes: [...base.nodes, { ...base.nodes.find((node) => node.name === "noise")!, name }],
  });
  const without = (base: Graph, name: string): Graph => ({
    ...base,
    nodes: base.nodes.filter((node) => node.name !== name),
    routes: base.routes.filter((route) => route.from.node !== name && route.to.node !== name),
  });

  it("takes a placement hint for its name", () => {
    const places = new Places();
    const first = places.place(graph, new Map());
    const spot = { x: 500, y: -40 };
    places.hint({ name: "white1", position: spot });
    const added = places.place(withNode(graph, "white1"), first);
    expect(added.get("white1")).toStrictEqual(spot);
    // Spent: dragged elsewhere, removed and added back, white1 keeps its known place, not the hint.
    const moved = new Map(added);
    moved.set("white1", { x: 7, y: 8 });
    const gone = places.place(graph, moved);
    gone.delete("white1");
    expect(places.place(withNode(graph, "white1"), gone).get("white1")).toStrictEqual({ x: 7, y: 8 });
  });

  it("drops a stale hint", () => {
    const places = new Places();
    const first = places.place(graph, new Map());
    places.hint({ name: "white1", position: { x: 500, y: -40 } });
    const renamed = withNode(graph, "white2");
    const second = places.place(renamed, first);
    expect(second.get("white2")).toStrictEqual(placeNodes(renamed, first).get("white2"));
    const both = withNode(renamed, "white1");
    expect(places.place(both, second).get("white1")).toStrictEqual(placeNodes(both, second).get("white1"));
  });

  it("a hint with no name takes the next new node", () => {
    const places = new Places();
    const first = places.place(graph, new Map());
    places.hint({ name: null, position: { x: 3, y: 4 } });
    // A replacement under a known name leaves the hint alone.
    const replaced = places.place(without(graph, "rec"), first);
    expect(places.place(graph, replaced).get("rec")).toStrictEqual(first.get("rec"));
    const added = places.place(withNode(graph, "file1"), places.place(graph, replaced));
    expect(added.get("file1")).toStrictEqual({ x: 3, y: 4 });
  });

  it("a hint beats a kept place", () => {
    const keeper: PlaceKeeper = { load: () => new Map([["rec", { x: 1, y: 1 }]]), save: () => {} };
    const places = new Places(keeper);
    places.hint({ name: "rec", position: { x: 9, y: 9 } });
    expect(places.place(graph, new Map()).get("rec")).toStrictEqual({ x: 9, y: 9 });
  });
});

describe("kept places", () => {
  function recording(loaded: Map<string, Position>) {
    const saves: { present: Map<string, Position>; forgetAbsent: boolean }[] = [];
    const keeper: PlaceKeeper = {
      load: () => loaded,
      save: (present, forgetAbsent) => saves.push({ present: new Map(present), forgetAbsent }),
    };
    return { keeper, saves };
  }

  it("loads kept places and saves only when they change", () => {
    const { keeper, saves } = recording(new Map([["rec", { x: -500, y: 900 }]]));
    const places = new Places(keeper);
    let state: GraphState = null;
    let current = new Map<string, Position>();
    let changes = 0;
    let key = "";
    for (const message of messagesOf(fixtures.lifecycle.messages)) {
      state = reduce(state, message);
      if (state === null) continue;
      current = places.place(state, current);
      const now = JSON.stringify([...current].sort(([a], [b]) => a.localeCompare(b)));
      if (now !== key) changes++;
      key = now;
    }
    expect(changes).toBeGreaterThan(1);
    expect(saves).toHaveLength(changes);
    expect(saves.every((save) => !save.forgetAbsent)).toBe(true);

    const fresh = new Places(recording(new Map([["rec", { x: -500, y: 900 }]])).keeper);
    expect(fresh.place(graph, new Map()).get("rec")).toStrictEqual({ x: -500, y: 900 });

    saves.length = 0;
    places.move(new Map([["desk", { x: 1, y: 2 }]]));
    places.move(new Map());
    expect(saves).toStrictEqual([{ present: new Map([["desk", { x: 1, y: 2 }]]), forgetAbsent: false }]);
    places.relayout(state!);
    expect(saves).toHaveLength(2);
    expect(saves[1]!.forgetAbsent).toBe(true);
  });

  it("quickPlace", () => {
    const point = { x: 400, y: 300 };
    expect(quickPlace(point, "pane")).toStrictEqual({ x: 400 - NODE_WIDTH / 2, y: 300 - NODE_HEADER / 2 });
    const wire = quickPlace(point, "wire");
    expect(wire.x).toBe(400);
    expect(wire.y + NODE_HEADER + NODE_PAD + NODE_ROW / 2).toBe(300);
  });
});
