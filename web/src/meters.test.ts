// The meter's rules (docs/editor.md#meters) on a fake monotonic clock: the zones, the peak hold and its fall, the clip
// lamp, the reset, and the tooltip.
import { describe, expect, it } from "vitest";
import { floorDb, fractionOfDb, meterFraction, toDbfs } from "./levels";
import { PeakHold, describeMeter, hotZoneDb, midZoneDb, zoneStops } from "./meters";
import type { Notifications } from "./protocol";
import { fixtures } from "./testFixtures";

const half = toDbfs(0.5); // -6.02 dBFS
const quarter = toDbfs(0.25); // -12.04 dBFS

describe("the meter", () => {
  it("zones start at -18 and -6 dBFS", () => {
    expect(midZoneDb).toBe(-18);
    expect(hotZoneDb).toBe(-6);
    expect(zoneStops.mid).toBeCloseTo(0.7, 10);
    expect(zoneStops.hot).toBeCloseTo(0.9, 10);
  });

  it("holds the highest peak for 1.5 s", () => {
    const hold = new PeakHold();
    hold.take(0.5, 0);
    hold.take(0.25, 100);
    for (const now of [100, 1000, 1499]) expect(hold.tickDb(now)).toBeCloseTo(half, 6);
    expect(hold.moving(1499)).toBe(true);
  });

  it("then falls at 20 dB/s", () => {
    const hold = new PeakHold();
    hold.take(0.5, 0);
    hold.take(0.01, 100); // the live peak at -40 dBFS, so the fall shows
    expect(hold.tickDb(2000)).toBeCloseTo(half - 10, 6);
    expect(hold.tickDb(2500)).toBeCloseTo(half - 20, 6);
    expect(hold.moving(2500)).toBe(true);
  });

  it("never falls below the live peak, and stops there", () => {
    const hold = new PeakHold();
    hold.take(0.5, 0);
    hold.take(0.25, 100);
    expect(hold.tickDb(3000)).toBeCloseTo(quarter, 6);
    expect(hold.moving(3000)).toBe(false);
  });

  it("a higher peak, or one at the falling tick, holds again", () => {
    const hold = new PeakHold();
    hold.take(0.5, 0);
    hold.take(0.01, 100);
    const falling = hold.tickDb(2000);
    expect(falling).toBeCloseTo(half - 10, 6);
    hold.take(0.2, 2000); // -13.98 dBFS, above the falling tick
    expect(hold.tickDb(2000)).toBeCloseTo(toDbfs(0.2), 6);
    hold.take(0.01, 2100); // the live peak back at -40 dBFS
    expect(hold.tickDb(3499)).toBeCloseTo(toDbfs(0.2), 6);
    expect(hold.tickDb(4000)).toBeCloseTo(toDbfs(0.2) - 10, 6);

    const again = new PeakHold();
    again.take(0.5, 0);
    again.take(0, 100);
    const tick = again.tickDb(2000);
    again.take(10 ** (tick / 20) * 1.000001, 2000); // a reading at the falling tick
    expect(again.tickDb(3499)).toBeCloseTo(tick, 4);
  });

  it("falls to the floor and is still", () => {
    const hold = new PeakHold();
    hold.take(0.5, 0);
    hold.take(0, 100);
    expect(hold.tickDb(10000)).toBe(floorDb);
    expect(hold.moving(10000)).toBe(false);
    expect(new PeakHold().moving(0)).toBe(false);
  });

  it("lights the clip lamp for 2 s at or over full scale", () => {
    const hold = new PeakHold();
    hold.take(1, 0);
    expect(hold.clipped(1999)).toBe(true);
    expect(hold.moving(1999)).toBe(true);
    expect(hold.clipped(2000)).toBe(false);

    const over = new PeakHold();
    over.take(1.3, 0);
    expect(over.clipped(0)).toBe(true);

    const under = new PeakHold();
    under.take(0.999, 0);
    expect(under.clipped(0)).toBe(false);
  });

  it("reset forgets the hold, the live peak and the lamp", () => {
    const hold = new PeakHold();
    hold.take(1, 0);
    hold.reset();
    expect(hold.tickDb(1)).toBe(floorDb);
    expect(hold.clipped(1)).toBe(false);
    expect(hold.moving(1)).toBe(false);
  });

  it("describeMeter", () => {
    expect(describeMeter([0, 0], new PeakHold(), 0)).toStrictEqual({
      rms: 0,
      peak: 0,
      clipped: false,
      title: "silent",
    });

    const hold = new PeakHold();
    hold.take(0.5, 0);
    const view = describeMeter([0.5, 0.25], hold, 0);
    expect(view.rms).toBe(meterFraction(0.25));
    expect(view.peak).toBeCloseTo(fractionOfDb(half), 10);
    expect(view.clipped).toBe(false);
    expect(view.title).toBe("peak -6.0 dBFS, rms -12.0 dBFS");

    const clip = new PeakHold();
    clip.take(1, 0);
    expect(describeMeter([1, 0.5], clip, 10).title).toBe("peak 0.0 dBFS, rms -6.0 dBFS; clipped");
    clip.take(0, 100);
    expect(describeMeter([0, 0], clip, 100).title).toBe("clipped");
    expect(describeMeter([0, 0], clip, 2100).title).toBe("silent");
  });

  it("replays the levels fixture", () => {
    const messages = fixtures.levels.messages
      .filter((message) => "method" in message && message.method === "levels")
      .map((message) => (message as { params: Notifications["levels"] }).params.nodes);
    expect(messages.length).toBeGreaterThan(1);
    const hold = new PeakHold();
    let highest = floorDb;
    messages.forEach((nodes, index) => {
      const now = index * 33;
      const peak = nodes.noise?.["noise.mono"]?.[0] ?? 0;
      hold.take(peak, now);
      highest = Math.max(highest, toDbfs(peak));
      expect(hold.tickDb(now)).toBeGreaterThanOrEqual(toDbfs(peak));
      expect(hold.tickDb(now)).toBeLessThanOrEqual(highest);
    });
  });
});
