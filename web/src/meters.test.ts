// A meter's words and scale (docs/editor.md#meters): the tooltip ear gives patchbay's Meter, and the defaults of the
// scale it draws on, which docs/editor.md states. The hold, the fall and the lamp are patchbay's Meter's, tested there.
import { defaultMeterScale, toDb } from "@mcurcio/patchbay/graph";
import { describe, expect, it } from "vitest";
import { floorDb } from "./levels";
import { meterTitle } from "./meters";

describe("the meter", () => {
  it("draws on patchbay's scale: -60 dBFS, zones at -18 and -6, a 1.5 s hold falling at 20 dB/s, a 2 s lamp", () => {
    expect(defaultMeterScale).toStrictEqual({
      floor: -60,
      zones: [-18, -6],
      holdMs: 1500,
      fallDbPerS: 20,
      clipAt: 1,
      clipHoldMs: 2000,
    });
    // The wires' scale (levels.ts) is the meters'.
    expect(floorDb).toBe(defaultMeterScale.floor);
  });

  it("says the peak and rms in dBFS, silent or clipped with nothing above the floor", () => {
    expect(meterTitle(-Infinity, -Infinity, false)).toBe("silent");
    expect(meterTitle(-70, -80, false)).toBe("silent");
    expect(meterTitle(toDb(0.5), toDb(0.25), false)).toBe("peak -6.0 dBFS, rms -12.0 dBFS");
    expect(meterTitle(toDb(1), toDb(0.5), true)).toBe("peak 0.0 dBFS, rms -6.0 dBFS; clipped");
    expect(meterTitle(toDb(0.5), -Infinity, false)).toBe("peak -6.0 dBFS, rms -60.0 dBFS");
    expect(meterTitle(-Infinity, -Infinity, true)).toBe("clipped");
  });
});
