// marks.ts against docs/editor.md#recent-marks: the one rule both marks follow, on values read straight from a graph
// (companions.test.ts and counters.test.ts prove each reading of the graph keeps it): a rise is a value above the one
// held; a first sighting, a fall and a value seen again after the graph dropped it are none; the minute and the sum of
// its rises; forgetting.
import { describe, expect, it } from "vitest";
import type { GraphState } from "./graph";
import { GraphMoves, isRecent, recentMarkMs, soonestExpiry, type HeldValues } from "./marks";

/** A GraphMoves reading one counter "c" of group "g" from a fake graph that is just its value (or none). */
type Fake = { c?: number } | null;
function moves(): { moves: GraphMoves; see: (value: number | undefined, now: number) => void } {
  const watched = new GraphMoves((graph: GraphState): HeldValues => {
    const value = (graph as unknown as Fake)?.c;
    return value === undefined ? new Map() : new Map([["g", new Map([["c", value]])]]);
  });
  return { moves: watched, see: (value, now) => watched.note({ c: value } as unknown as GraphState, now) };
}

describe("the mark's one rule", () => {
  const at = 500_000;

  it("takes a value above the one held as a rise, and nothing else", () => {
    const { moves: m, see } = moves();
    see(5, at);
    expect(m.movedAt("g", "c")).toBeUndefined();
    see(5, at + 1);
    expect(m.movedAt("g", "c")).toBeUndefined();
    see(7, at + 2);
    expect(m.movedAt("g", "c")).toBe(at + 2);
    expect(m.risenBy("g", "c", at + 2)).toBe(2);
  });

  it("takes a fall as a first value: it clears the minute's rises", () => {
    const { moves: m, see } = moves();
    see(0, at);
    see(4, at + 1);
    see(1, at + 2);
    expect(m.movedAt("g", "c")).toBeUndefined();
    expect(m.risenBy("g", "c", at + 2)).toBe(0);
    see(3, at + 3);
    expect(m.risenBy("g", "c", at + 3)).toBe(2);
  });

  it("forgets a value the graph dropped, so its return is a first sighting", () => {
    const { moves: m, see } = moves();
    see(0, at);
    see(undefined, at + 1);
    see(9, at + 2);
    expect(m.movedAt("g", "c")).toBeUndefined();
  });

  it("forgets everything on forget()", () => {
    const { moves: m, see } = moves();
    see(0, at);
    m.forget();
    see(9, at + 1);
    expect(m.movedAt("g", "c")).toBeUndefined();
    expect(m.risenBy("g", "c", at + 1)).toBe(0);
  });

  it("sums the rises of the last minute, each leaving at its own minute's end", () => {
    const { moves: m, see } = moves();
    see(0, at);
    see(2, at + 10);
    see(5, at + 20);
    expect(m.risenBy("g", "c", at + 20)).toBe(5);
    expect(m.nextLeave("g", "c", at + 20)).toBe(at + 10 + recentMarkMs);
    expect(m.risenBy("g", "c", at + 10 + recentMarkMs)).toBe(3);
    expect(m.nextLeave("g", "c", at + 10 + recentMarkMs)).toBe(at + 20 + recentMarkMs);
    expect(m.risenBy("g", "c", at + 20 + recentMarkMs)).toBe(0);
    expect(m.nextLeave("g", "c", at + 20 + recentMarkMs)).toBeUndefined();
  });

  it("drops a rise from the sum once its minute passed, at the next note too", () => {
    const { moves: m, see } = moves();
    see(0, at);
    see(2, at + 10);
    see(3, at + 10 + recentMarkMs);
    expect(m.risenBy("g", "c", at + 10 + recentMarkMs)).toBe(1);
  });

  it("times the minute end-exclusive", () => {
    expect(isRecent(at, at + recentMarkMs - 1)).toBe(true);
    expect(isRecent(at, at + recentMarkMs)).toBe(false);
    expect(isRecent(undefined, at)).toBe(false);
    expect(soonestExpiry([at + 5, undefined, at], at + 1)).toBe(at + recentMarkMs);
    expect(soonestExpiry([at - recentMarkMs], at)).toBeUndefined();
  });
});
