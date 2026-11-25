// What a wire draws (docs/editor.md#wires), replaying the snapshot's routes and the unknown-values fixture's unknown
// status through the reducer.
import { describe, expect, it } from "vitest";
import { reduce, type GraphState } from "./graph";
import type { GraphRoute } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";
import { wireEnds, wireStatusWords, wireView } from "./wires";

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
  it("draws each status by its class, an unknown one as pending", () => {
    expect(snapshotRoutes.map((route) => wireView(route, 0, false).statusClass)).toEqual([
      "wire-pending",
      "wire-linked",
      "wire-refused",
    ]);
    expect(wireView(held, 0, false).statusClass).toBe("wire-unknown");
  });

  it("dashes every status but linked", () => {
    expect(wireView(linked, 0.5, false).dashed).toBe(false);
    for (const route of [pending, refused, held]) expect(wireView(route, 0.5, false).dashed).toBe(true);
  });

  it("flows only on a linked wire above the floor", () => {
    expect(wireView(linked, 0.5, false).flowing).toBe(true);
    for (const rms of [0, 0.0001, 0.001]) expect(wireView(linked, rms, false).flowing).toBe(false);
    for (const route of [pending, refused, held]) expect(wireView(route, 1, false).flowing).toBe(false);
  });

  it("thickness and brightness follow the level, and selection", () => {
    expect(wireView(linked, 0, false)).toMatchObject({ width: 1.5, opacity: 0.45 });
    expect(wireView(linked, 1, false)).toMatchObject({ width: 6.5, opacity: 1 });
    expect(wireView(linked, 1, true).width).toBe(8);
    expect(wireView(linked, 0, true).width).toBe(3);
  });

  it("labels the status in words where colour is not enough", () => {
    expect(wireView(refused, 0, false).label).toBe("refused");
    expect(wireView(held, 0, false).label).toBe("held");
    expect(wireView(linked, 0, false).label).toBeUndefined();
    expect(wireView(pending, 0, false).label).toBeUndefined();
  });

  it("titles the wire by its two ends", () => {
    const ends = wireEnds({ from: { node: "noise", port: "noise.mono" }, to: { node: "rec", port: "record.right" } });
    expect(ends).toBe("noise:noise.mono → rec:record.right");
    expect(refused.detail).toBeDefined();
    expect(wireView(refused, 0, false).title).toBe(`${wireEnds(refused)}\nrefused: ${refused.detail}`);
    expect(wireView(linked, 0, false).title).toBe(wireEnds(linked));
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
