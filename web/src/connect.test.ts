// An output → input drag gives route.add's params; input → input (and
// output → output) is refused.
import { describe, expect, it } from "vitest";
import { edgeId, handleCue, handleId, isValidConnection, parseHandleId, routeFromConnection } from "./connect";
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

describe("the cue while a wire is drawn", () => {
  const handles = fixtures.snapshot.graph.nodes.flatMap((node) => [
    ...node.inputs.map((port) => ({ node: node.name, id: inp(port) })),
    ...node.outputs.map((port) => ({ node: node.name, id: out(port) })),
  ]);

  it("leaves every handle idle with no drag", () => {
    for (const handle of handles) expect(handleCue(null, handle.node, handle.id)).toBe("idle");
  });

  it("marks the drag's own handle, rings where a drop makes a route and dims the rest", () => {
    const fromNoise = { nodeId: "noise", id: out("noise.mono") };
    expect(handleCue(fromNoise, "noise", out("noise.mono"))).toBe("origin");
    expect(handleCue(fromNoise, "rec", inp("record.left"))).toBe("target");
    expect(handleCue(fromNoise, "quiet", out("noise.mono"))).toBe("dimmed");
    expect(handleCue(fromNoise, "noise", out("other"))).toBe("dimmed");

    const fromRec = { nodeId: "rec", id: inp("record.left") };
    expect(handleCue(fromRec, "noise", out("noise.mono"))).toBe("target");
    expect(handleCue(fromRec, "tape", inp("record.right"))).toBe("dimmed");
    expect(handleCue(fromRec, "rec", inp("record.right"))).toBe("dimmed");
  });

  it("is the drop rule for every pair of the snapshot's handles", () => {
    expect(handles.length).toBeGreaterThan(4);
    for (const from of handles) {
      for (const to of handles) {
        if (from.node === to.node && from.id === to.id) continue;
        const pair = { source: from.node, sourceHandle: from.id, target: to.node, targetHandle: to.id };
        const valid = isValidConnection(pair);
        expect(handleCue({ nodeId: from.node, id: from.id }, to.node, to.id) === "target").toBe(valid);
      }
    }
  });

  it("keeps dotted and colon names whole", () => {
    const from = { nodeId: "kitchen", id: out("from.left") };
    expect(handleCue(from, "desk", inp("odd:port.1"))).toBe("target");
    expect(handleCue({ nodeId: "desk", id: inp("odd:port.1") }, "desk", inp("odd:port.1"))).toBe("origin");
  });
});
