// First placement: unplaced nodes get a dagre position, sources left of their sinks; a node the
// user placed keeps its position.
import { describe, expect, it } from "vitest";
import { reduce, type GraphState } from "./graph";
import editorCss from "./editor.css?raw";
import { Places, headerHeight, nodeSize, nodeWidth, placeNodes, portHeight, type Position } from "./layout";
import type { GraphNode } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";

const graph = fixtures.snapshot.graph;

describe("a box's size", () => {
  const node = (inputs: number, outputs: number): GraphNode => ({
    ...graph.nodes[0]!,
    inputs: Array.from({ length: inputs }, (_, i) => `in.${i + 1}`),
    outputs: Array.from({ length: outputs }, (_, i) => `out.${i + 1}`),
  });

  it("is 220 px wide, one row per port on its longer side", () => {
    expect(nodeWidth).toBe(220);
    expect(nodeSize(node(4, 1))).toStrictEqual({ width: 220, height: headerHeight + 4 * portHeight });
    expect(nodeSize(node(0, 0))).toStrictEqual({ width: 220, height: headerHeight + portHeight });
  });

  it("is the box's CSS", () => {
    const css = editorCss.replace(/\/\*[\s\S]*?\*\//g, "");
    const declaration = (selector: string, property: string): string => {
      const rule = new RegExp(`(?:^|\\})\\s*${selector.replace(/[.]/g, "\\.")}\\s*\\{([^}]*)\\}`).exec(css);
      if (rule === null) throw new Error(`editor.css has no ${selector} rule`);
      const value = new RegExp(`(?:^|;)\\s*${property}\\s*:\\s*([^;]+)`).exec(rule[1]!);
      if (value === null) throw new Error(`editor.css's ${selector} rule has no ${property}`);
      return value[1]!.trim();
    };
    expect(declaration(".ear-node", "width")).toBe(`${nodeWidth}px`);
    expect(declaration(".port", "height")).toBe(`${portHeight}px`);
  });
});

describe("the first layout", () => {
  it("places every node, each source left of the nodes it feeds", () => {
    const positions = placeNodes(graph, new Map());
    expect([...positions.keys()].sort()).toEqual(graph.nodes.map((node) => node.name));
    for (const route of graph.routes) {
      expect(positions.get(route.from.node)!.x + nodeWidth).toBeLessThan(positions.get(route.to.node)!.x);
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

// ear replaces a node whose ports change (node.removed, then node.added); the
// canvas may draw the graph between the two, and the node keeps its place.
describe("places", () => {
  it("keeps a replaced node's place, drawn after every message of the graph-file fixture", () => {
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
    expect(deskSeenGone).toBe(true);
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
