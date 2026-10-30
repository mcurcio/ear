// First placement: unplaced nodes get a dagre position, sources left of their sinks; a node the
// user placed keeps its position.
import { describe, expect, it } from "vitest";
import { nodeWidth, placeNodes } from "./layout";
import { fixtures } from "./testFixtures";

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
