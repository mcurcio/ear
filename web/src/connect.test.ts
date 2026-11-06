// W1 criterion (3): an output → input drag gives route.add's params; input → input (and
// output → output) is refused.
import { describe, expect, it } from "vitest";
import { edgeId, handleId, isValidConnection, parseHandleId, routeFromConnection } from "./connect";

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
