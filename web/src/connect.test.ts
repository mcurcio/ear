// An output → input drag gives route.add's params; input → input (and
// output → output) is refused.
import { describe, expect, it } from "vitest";
import {
  canConnect,
  edgeId,
  handleId,
  isValidConnection,
  parseHandleId,
  quickAddKind,
  routeFromConnection,
} from "./connect";
import { fixtures } from "./testFixtures";

const out = (port: string) => handleId("out", port);
const inp = (port: string) => handleId("in", port);

describe("handle ids and dotted port names", () => {
  it("keeps a bank and its member together, and splits at the first colon only", () => {
    expect(handleId("out", "from.left")).toBe("out:from.left");
    expect(parseHandleId(handleId("in", "play.1"))).toEqual({ direction: "in", port: "play.1" });
    expect(parseHandleId("in:odd:port.1")).toEqual({ direction: "in", port: "odd:port.1" });
  });

  it("makes a route from two dotted handles, and a wire id of the whole names", () => {
    const route = routeFromConnection({ source: "kitchen", sourceHandle: out("from.left"), target: "desk", targetHandle: inp("play.1") });
    expect(route).toEqual({ from: { node: "kitchen", port: "from.left" }, to: { node: "desk", port: "play.1" } });
    expect(edgeId(route!)).toBe('["kitchen","from.left","desk","play.1"]');
  });
});

describe("the connection rules", () => {
  it("turn an output → input drag into route.add's params", () => {
    const connection = { source: "noise", sourceHandle: out("noise.mono"), target: "rec", targetHandle: inp("record.right") };
    expect(routeFromConnection(connection)).toStrictEqual({
      from: { node: "noise", port: "noise.mono" },
      to: { node: "rec", port: "record.right" },
    });
    expect(isValidConnection(connection)).toBe(true);
  });

  it("route from the output whichever end the drag started at", () => {
    const connection = { source: "rec", sourceHandle: inp("record.right"), target: "noise", targetHandle: out("noise.mono") };
    expect(routeFromConnection(connection)).toStrictEqual({
      from: { node: "noise", port: "noise.mono" },
      to: { node: "rec", port: "record.right" },
    });
  });

  it("refuse input → input and output → output", () => {
    const inputs = { source: "rec", sourceHandle: inp("record.left"), target: "tape", targetHandle: inp("record.right") };
    const outputs = { source: "noise", sourceHandle: out("noise.mono"), target: "n2", targetHandle: out("noise.mono") };
    expect(routeFromConnection(inputs)).toBeNull();
    expect(isValidConnection(inputs)).toBe(false);
    expect(routeFromConnection(outputs)).toBeNull();
  });

  it("refuse a drag without port handles", () => {
    expect(routeFromConnection({ source: "noise", sourceHandle: null, target: "rec", targetHandle: inp("record.left") })).toBeNull();
    expect(parseHandleId("noise.mono")).toBeNull();
    expect(parseHandleId("sideways:noise.mono")).toBeNull();
  });

  it("keep port names that hold a colon", () => {
    expect(parseHandleId(out("a:b"))).toStrictEqual({ direction: "out", port: "a:b" });
  });

  it("give each route one wire id", () => {
    const route = { from: { node: "noise", port: "noise.mono" }, to: { node: "rec", port: "record.left" } };
    expect(edgeId(route)).toBe(edgeId(structuredClone(route)));
    expect(edgeId(route)).not.toBe(edgeId({ ...route, to: { node: "rec", port: "record.right" } }));
  });
});

// The rule patchbay's useConnectHints judges each handle by while a wire is drawn, and React Flow the drop by
// (docs/editor.md#the-node-box): the origin, ok and no hints are patchbay's, the verdicts ear's.
describe("the rule the hints and the drop share", () => {
  const handles = fixtures.snapshot.graph.nodes.flatMap((node) => [
    ...node.inputs.map((port) => ({ nodeId: node.name, handleId: inp(port) })),
    ...node.outputs.map((port) => ({ nodeId: node.name, handleId: out(port) })),
  ]);

  it("accepts an output joined to an input, and refuses two outputs or two inputs", () => {
    expect(canConnect({ nodeId: "noise", handleId: out("noise.mono") }, { nodeId: "rec", handleId: inp("record.left") })).toBe(true);
    expect(canConnect({ nodeId: "noise", handleId: out("noise.mono") }, { nodeId: "quiet", handleId: out("noise.mono") })).toBe(false);
    expect(canConnect({ nodeId: "rec", handleId: inp("record.left") }, { nodeId: "tape", handleId: inp("record.right") })).toBe(false);
    expect(canConnect({ nodeId: "noise", handleId: null }, { nodeId: "rec", handleId: inp("record.left") })).toBe(false);
  });

  it("is the drop rule for every pair of the snapshot's handles", () => {
    expect(handles.length).toBeGreaterThan(4);
    for (const from of handles) {
      for (const to of handles) {
        const pair = { source: from.nodeId, sourceHandle: from.handleId, target: to.nodeId, targetHandle: to.handleId };
        expect(canConnect(from, to)).toBe(isValidConnection(pair));
      }
    }
  });

  it("keeps dotted and colon names whole", () => {
    expect(canConnect({ nodeId: "kitchen", handleId: out("from.left") }, { nodeId: "desk", handleId: inp("odd:port.1") })).toBe(true);
  });
});

// Quick add (docs/editor.md#adding-on-the-canvas): patchbay's useAddAt finds the double-click or the drop on empty
// canvas; ear follows an output's wire only.
describe("quickAddKind", () => {
  it("is the pane's for a double-click and a wire's for an output's drop, and none for an input's", () => {
    expect(quickAddKind()).toBe("pane");
    expect(quickAddKind({ side: "output" })).toBe("wire");
    expect(quickAddKind({ side: "input" })).toBeNull();
  });
});
