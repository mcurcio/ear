// W1 criterion (4): dBFS conversion and clamp; a wire's level is its from port's; a port update
// notifies only that port's listeners; a node missing from the next message reads silent.
import { describe, expect, it } from "vitest";
import { LevelStore, floorDb, meterFraction, toDbfs, wireLevel } from "./levels";
import type { Notifications } from "./protocol";
import { fixtures } from "./testFixtures";

const levelMessages = fixtures.levels.messages
  .filter((message) => "method" in message && message.method === "levels")
  .map((message) => (message as { params: Notifications["levels"] }).params.nodes);

describe("dBFS", () => {
  it("converts linear levels and clamps at -60", () => {
    expect(toDbfs(1)).toBe(0);
    expect(toDbfs(0.5)).toBeCloseTo(-6.0206, 4);
    expect(toDbfs(0.001)).toBeCloseTo(-60, 10);
    expect(toDbfs(0.0001)).toBe(floorDb);
    expect(toDbfs(0)).toBe(floorDb);
    expect(meterFraction(0)).toBe(0);
    expect(meterFraction(1)).toBe(1);
    expect(meterFraction(2)).toBe(1);
    expect(meterFraction(0.5)).toBeCloseTo(1 - 6.0206 / 60, 4);
  });
});

describe("the level store", () => {
  it("draws a wire with its from port's level", () => {
    const store = new LevelStore();
    store.replace(levelMessages[0]!);
    const route = { from: { node: "noise", port: "MONO" }, to: { node: "rec", port: "FR" } };
    expect(wireLevel(store, route)).toEqual([0.9998, 0.5771]);
  });

  it("is replaced by each levels message: a node missing from it reads silent", () => {
    const store = new LevelStore();
    store.replace(levelMessages[0]!);
    expect(store.get("noise", "MONO")).toEqual([0.9998, 0.5771]);
    store.replace(levelMessages[1]!);
    expect(store.get("noise", "MONO")).toEqual([0, 0]);
    expect(store.get("rec", "FL")).toEqual([0.9991, 0.5779]);
    expect(store.get("nobody", "X")).toEqual([0, 0]);
  });

  it("tells only the listeners of a port whose level changed", () => {
    const store = new LevelStore();
    const told: string[] = [];
    store.subscribe("noise", "MONO", () => told.push("noise:MONO"));
    store.subscribe("rec", "FL", () => told.push("rec:FL"));
    store.subscribe("rec", "FR", () => told.push("rec:FR"));
    store.replace({ noise: { MONO: [0.5, 0.25] }, rec: { FL: [0.5, 0.25], FR: [0, 0] } });
    expect(told.sort()).toEqual(["noise:MONO", "rec:FL"]);
    told.length = 0;
    store.replace({ noise: { MONO: [0.5, 0.25] }, rec: { FL: [0.4, 0.2], FR: [0, 0] } });
    expect(told).toEqual(["rec:FL"]);
    told.length = 0;
    // noise is gone from the next message: its meter goes silent and is told so.
    store.replace({ rec: { FL: [0.4, 0.2], FR: [0, 0] } });
    expect(told).toEqual(["noise:MONO"]);
  });

  it("stops telling a listener after it unsubscribes", () => {
    const store = new LevelStore();
    let told = 0;
    const unsubscribe = store.subscribe("noise", "MONO", () => told++);
    unsubscribe();
    store.replace({ noise: { MONO: [0.5, 0.25] } });
    expect(told).toBe(0);
  });
});
