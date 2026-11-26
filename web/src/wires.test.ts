// What a wire draws (docs/editor.md#wires), replaying the snapshot's routes and the unknown-values fixture's unknown
// status through the reducer.
import { describe, expect, it } from "vitest";
import { reduce, type GraphState } from "./graph";
import type { GraphRoute } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";
import { meterFraction } from "./levels";
import { wireData, wireEnds, wireStatusClass, wireStatusWords } from "./wires";

function routesAfter(fixture: { messages: Parameters<typeof messagesOf>[0] }): GraphRoute[] {
  let state: GraphState = null;
  for (const message of messagesOf(fixture.messages)) state = reduce(state, message);
  return state!.routes;
}

const snapshotRoutes = routesAfter(fixtures.snapshot);
const byStatus = (status: string): GraphRoute => {
  const found = [...snapshotRoutes, ...routesAfter(fixtures.unknownValues)].find((route) => route.status === status);
  if (found === undefined) throw new Error(`no ${status} route in the fixtures`);
  return found;
};
const linked = byStatus("linked");
const pending = byStatus("pending");
const refused = byStatus("refused");
const held = byStatus("held");

describe("a wire", () => {
  it("draws each status by its class and tone, an unknown one as pending", () => {
    expect(snapshotRoutes.map(wireStatusClass)).toEqual(["wire-pending", "wire-linked", "wire-refused"]);
    expect(wireStatusClass(held)).toBe("wire-unknown");
    expect([linked, pending, refused, held].map((route) => wireData(route, 0).tone)).toEqual([
      "ok",
      "pending",
      "refused",
      "pending",
    ]);
  });

  it("dashes every status but linked", () => {
    expect(wireData(linked, 0.5).dashed).toBe(false);
    for (const route of [pending, refused, held]) expect(wireData(route, 0.5).dashed).toBe(true);
  });

  it("flows only on a linked wire above the floor", () => {
    expect(wireData(linked, 0.5).flow).toBe(true);
    for (const rms of [0, 0.0001, 0.001]) expect(wireData(linked, rms).flow).toBe(false);
    for (const route of [pending, refused, held]) expect(wireData(route, 1).flow).toBe(false);
  });

  it("is emphasised by its from port's level on the meter's scale", () => {
    expect(wireData(linked, 0).emphasis).toBe(0);
    expect(wireData(linked, 1).emphasis).toBe(1);
    expect(wireData(linked, 0.1).emphasis).toBe(meterFraction(0.1));
    expect(wireData(pending, 0.1).emphasis).toBe(meterFraction(0.1));
  });

  it("labels the status in words where colour is not enough", () => {
    expect(wireData(refused, 0).label).toBe("refused");
    expect(wireData(held, 0).label).toBe("held");
    expect(wireData(linked, 0).label).toBeUndefined();
    expect(wireData(pending, 0).label).toBeUndefined();
  });

  it("titles the wire by its two ends", () => {
    const ends = wireEnds({ from: { node: "noise", port: "noise.mono" }, to: { node: "rec", port: "record.right" } });
    expect(ends).toBe("noise:noise.mono → rec:record.right");
    expect(refused.detail).toBeDefined();
    expect(wireData(refused, 0).title).toBe(`${wireEnds(refused)}\nrefused: ${refused.detail}`);
    expect(wireData(linked, 0).title).toBe(wireEnds(linked));
    expect(wireEnds({ from: { node: "kitchen", port: "from.left" }, to: { node: "desk", port: "play.1" } })).toBe(
      "kitchen:from.left → desk:play.1",
    );
  });

  it("says its status in words for the inspector", () => {
    expect(wireStatusWords(linked)).toBe("linked");
    expect(wireStatusWords(pending)).toBe("pending: ear has not linked it yet");
    expect(wireStatusWords(refused)).toBe(`refused: ${refused.detail}`);
    const { detail: _detail, ...bare } = refused;
    expect(wireStatusWords(bare)).toBe("refused");
    expect(wireStatusWords(held)).toBe("held");
  });
});
