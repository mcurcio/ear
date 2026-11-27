// The tapers and their inverses (docs/editor.md#faders-and-knobs): linear, log and db map travel to a value and back;
// a dB unit does not imply the db taper; the words a control shows; a key's step.
import { describe, expect, it } from "vitest";
import { dbPoints, roundedValue, taperOf, travelOf, valueAfterKey, valueAt, valueText, type TaperRow } from "./taper";

const gainDb: TaperRow = { min: -90, max: 24, taper: "db", unit: "dB", decimals: 1, minIsSilence: true };
const trimDb: TaperRow = { min: -24, max: 24, taper: "linear", unit: "dB", decimals: 1 };
const freqHz: TaperRow = { min: 20, max: 20000, taper: "log", unit: "Hz", decimals: 0 };

describe("the tapers", () => {
  it("linear is min + t × (max − min)", () => {
    expect(valueAt(trimDb, 0)).toBe(-24);
    expect(valueAt(trimDb, 0.5)).toBe(0);
    expect(valueAt(trimDb, 1)).toBe(24);
    expect(valueAt(trimDb, 0.25)).toBe(-12);
  });

  it("log is min × (max / min)^t", () => {
    expect(valueAt(freqHz, 0)).toBeCloseTo(20, 9);
    expect(valueAt(freqHz, 1)).toBeCloseTo(20000, 6);
    expect(valueAt(freqHz, 0.5)).toBeCloseTo(Math.sqrt(20 * 20000), 6);
  });

  it("db goes through its points, min at 0 and max at 1", () => {
    expect(valueAt(gainDb, 0)).toBe(-90);
    expect(valueAt(gainDb, 1)).toBe(24);
    for (const [t, db] of dbPoints) expect(valueAt(gainDb, t)).toBeCloseTo(db, 9);
    // Linear between two points.
    expect(valueAt(gainDb, 0.675)).toBeCloseTo(-5, 9);
    expect(valueAt(gainDb, 0.05)).toBeCloseTo(-75, 9);
  });

  it("each inverse gives back the travel, and clamps outside the range", () => {
    for (const row of [gainDb, trimDb, freqHz]) {
      for (let i = 0; i <= 100; ++i) expect(travelOf(row, valueAt(row, i / 100))).toBeCloseTo(i / 100, 9);
    }
    expect(travelOf(gainDb, -200)).toBe(0);
    expect(travelOf(gainDb, 99)).toBe(1);
    expect(valueAt(trimDb, -1)).toBe(-24);
    expect(valueAt(trimDb, 2)).toBe(24);
  });

  it("a dB unit does not imply the db taper, nor a range that does not hold its points", () => {
    expect(taperOf(trimDb)).toBe("linear");
    expect(taperOf({ ...trimDb, taper: "db" })).toBe("linear");
    expect(taperOf({ min: -40, max: 12, taper: "db" })).toBe("linear");
    expect(taperOf(gainDb)).toBe("db");
    expect(taperOf({ min: 0, max: 10, taper: "log" })).toBe("linear");
    expect(taperOf(freqHz)).toBe("log");
    expect(taperOf({ min: 0, max: 1 })).toBe("linear");
  });

  it("shows the row's places and unit, and -inf at a silent min", () => {
    expect(valueText(gainDb, -3.5)).toBe("-3.5 dB");
    expect(valueText(gainDb, -3.46)).toBe("-3.5 dB");
    expect(valueText(gainDb, -90)).toBe("-inf dB");
    expect(valueText(trimDb, -24)).toBe("-24.0 dB");
    expect(valueText(trimDb, -0.04)).toBe("0.0 dB");
    expect(valueText(freqHz, 1000.4)).toBe("1000 Hz");
    expect(roundedValue(gainDb, -3.45)).toBe(-3.5);
    expect(roundedValue(gainDb, 3.45)).toBe(3.5);
    expect(roundedValue(gainDb, 30)).toBe(24);
  });

  it("an arrow steps 1% of the travel (Shift 0.1%), Page Up and Down 10%, Home and End the ends", () => {
    expect(valueAfterKey(trimDb, 0, "ArrowUp", false)).toBeCloseTo(0.48, 9);
    expect(valueAfterKey(trimDb, 0, "ArrowRight", true)).toBeCloseTo(0.048, 9);
    expect(valueAfterKey(trimDb, 0, "ArrowDown", false)).toBeCloseTo(-0.48, 9);
    expect(valueAfterKey(trimDb, 0, "PageUp", false)).toBeCloseTo(4.8, 9);
    expect(valueAfterKey(trimDb, 0, "PageDown", false)).toBeCloseTo(-4.8, 9);
    expect(valueAfterKey(trimDb, 0, "Home", false)).toBe(-24);
    expect(valueAfterKey(trimDb, 0, "End", false)).toBe(24);
    expect(valueAfterKey(trimDb, 24, "ArrowUp", false)).toBe(24);
    expect(valueAfterKey(trimDb, 0, "Backspace", false)).toBeUndefined();
    expect(valueAfterKey(trimDb, 0, "m", false)).toBeUndefined();
    // On the db taper a step is of the travel: from 0 dB (t 0.75), 1% up is +0.96 dB.
    expect(valueAfterKey(gainDb, 0, "ArrowUp", false)).toBeCloseTo(0.96, 9);
  });
});
