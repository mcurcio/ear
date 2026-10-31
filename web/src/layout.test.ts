// First placement: unplaced nodes get a dagre position, sources left of their sinks; a node the
// user placed keeps its position.
import { describe, expect, it } from "vitest";
import { reduce, type GraphState } from "./graph";
import { Places, nodeWidth, placeNodes, type Position } from "./layout";
import { fixtures, messagesOf } from "./testFixtures";

const graph = fixtures.snapshot.graph;

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

// S6 criterion 5: ear replaces a node whose ports change (node.removed, then node.added); the
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
});
