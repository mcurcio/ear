// The inspector's subject and Delete's requests (docs/editor.md#the-inspector), against the snapshot fixture's graph.
import { describe, expect, it } from "vitest";
import { edgeId } from "./connect";
import {
  nothingSelected,
  removals,
  selectionKey,
  selectionOf,
  selectionText,
  subjectOf,
  subjectTitle,
  withSelected,
} from "./selection";
import { fixtures } from "./testFixtures";

const graph = fixtures.snapshot.graph;
const [pending, linked] = graph.routes;

describe("the inspector's subject", () => {
  it("is the graph, a node, a route or several", () => {
    expect(subjectOf(graph, nothingSelected)).toStrictEqual({ kind: "graph" });
    const node = subjectOf(graph, { nodes: ["rec"], routes: [] });
    expect(node.kind).toBe("node");
    expect(node.kind === "node" && node.node).toBe(graph.nodes.find((n) => n.name === "rec"));
    const route = subjectOf(graph, { nodes: [], routes: [edgeId(linked!)] });
    expect(route.kind === "route" && route.route).toBe(linked);
    const several = subjectOf(graph, { nodes: ["rec", "noise"], routes: [edgeId(linked!)] });
    expect(several).toStrictEqual({
      kind: "several",
      nodes: [graph.nodes.find((n) => n.name === "noise"), graph.nodes.find((n) => n.name === "rec")],
      routes: [linked],
      text: "2 nodes, 1 route",
    });
    expect(subjectTitle(several)).toBe("2 nodes, 1 route");
    expect(subjectTitle(node)).toBe("rec");
    expect(subjectTitle(route)).toBe("Route");
    expect(subjectTitle({ kind: "graph" })).toBe("Graph");
  });

  it("ignores a name or wire id the graph does not hold", () => {
    expect(subjectOf(graph, { nodes: ["gone"], routes: [] })).toStrictEqual({ kind: "graph" });
    const ghost = edgeId({ from: { node: "gone", port: "a" }, to: { node: "rec", port: "record.left" } });
    expect(subjectOf(graph, { nodes: [], routes: [ghost] })).toStrictEqual({ kind: "graph" });
    expect(subjectOf(graph, { nodes: ["gone", "rec"], routes: [ghost] }).kind).toBe("node");
  });
});

describe("the count in words", () => {
  it("names nodes and routes, and nothing for nothing", () => {
    expect(selectionText(1, 0)).toBe("1 node");
    expect(selectionText(3, 0)).toBe("3 nodes");
    expect(selectionText(0, 1)).toBe("1 route");
    expect(selectionText(0, 2)).toBe("2 routes");
    expect(selectionText(3, 2)).toBe("3 nodes, 2 routes");
    expect(selectionText(0, 0)).toBe("");
  });
});

describe("Delete's requests", () => {
  it("removes each node, and only routes between kept nodes", () => {
    const between = { from: { node: "quiet", port: "out" }, to: { node: "rec", port: "record.right" } };
    expect(removals(["noise"], [pending!, linked!, between])).toStrictEqual({ nodes: ["noise"], routes: [between] });
    expect(removals([], [linked!])).toStrictEqual({ nodes: [], routes: [linked] });
    expect(removals(["rec", "rec"], [])).toStrictEqual({ nodes: ["rec"], routes: [] });
  });
});

describe("the canvas's selection", () => {
  const nodes = [
    { id: "a", selected: true },
    { id: "b" },
    { id: "c", selected: false },
  ];

  it("sets selected exactly for the names given, keeping unchanged nodes' objects", () => {
    const next = withSelected(nodes, new Set(["b"]));
    expect(next.map((node) => node.selected === true)).toEqual([false, true, false]);
    expect(next[2]).toBe(nodes[2]);
    expect(next[0]).not.toBe(nodes[0]);
    const same = withSelected(nodes, new Set(["a"]));
    expect(same.every((node, index) => node === nodes[index])).toBe(true);
  });

  it("reads the selection in canvas order, with a key stable across order", () => {
    const selection = selectionOf(nodes, new Set(["w2", "w1"]));
    expect(selection).toStrictEqual({ nodes: ["a"], routes: ["w2", "w1"] });
    expect(selectionKey(selection)).toBe(selectionKey({ nodes: ["a"], routes: ["w1", "w2"] }));
    expect(selectionKey({ nodes: ["a", "b"], routes: [] })).toBe(selectionKey({ nodes: ["b", "a"], routes: [] }));
    expect(selectionKey(nothingSelected)).toBe("");
    expect(selectionKey({ nodes: ["a"], routes: [] })).not.toBe(selectionKey({ nodes: [], routes: ["a"] }));
  });
});
