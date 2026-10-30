// W1 criterion (3): an output → input drag gives route.add's params; input → input (and
// output → output) is refused.
import { describe, expect, it } from "vitest";
import { edgeId, handleId, isValidConnection, parseHandleId, routeFromConnection } from "./connect";

const out = (port: string) => handleId("out", port);
const inp = (port: string) => handleId("in", port);

describe("the connection rules", () => {
  it("turn an output → input drag into route.add's params", () => {
    const connection = { source: "noise", sourceHandle: out("MONO"), target: "rec", targetHandle: inp("FR") };
    expect(routeFromConnection(connection)).toStrictEqual({
      from: { node: "noise", port: "MONO" },
      to: { node: "rec", port: "FR" },
    });
    expect(isValidConnection(connection)).toBe(true);
  });

  it("route from the output whichever end the drag started at", () => {
    const connection = { source: "rec", sourceHandle: inp("FR"), target: "noise", targetHandle: out("MONO") };
    expect(routeFromConnection(connection)).toStrictEqual({
      from: { node: "noise", port: "MONO" },
      to: { node: "rec", port: "FR" },
    });
  });

  it("refuse input → input and output → output", () => {
    const inputs = { source: "rec", sourceHandle: inp("FL"), target: "tape", targetHandle: inp("FR") };
    const outputs = { source: "noise", sourceHandle: out("MONO"), target: "n2", targetHandle: out("MONO") };
    expect(routeFromConnection(inputs)).toBeNull();
    expect(isValidConnection(inputs)).toBe(false);
    expect(routeFromConnection(outputs)).toBeNull();
  });

  it("refuse a drag without port handles", () => {
    expect(routeFromConnection({ source: "noise", sourceHandle: null, target: "rec", targetHandle: inp("FL") })).toBeNull();
    expect(parseHandleId("MONO")).toBeNull();
    expect(parseHandleId("sideways:MONO")).toBeNull();
  });

  it("keep port names that hold a colon", () => {
    expect(parseHandleId(out("a:b"))).toStrictEqual({ direction: "out", port: "a:b" });
  });

  it("give each route one wire id", () => {
    const route = { from: { node: "noise", port: "MONO" }, to: { node: "rec", port: "FL" } };
    expect(edgeId(route)).toBe(edgeId(structuredClone(route)));
    expect(edgeId(route)).not.toBe(edgeId({ ...route, to: { node: "rec", port: "FR" } }));
  });
});
