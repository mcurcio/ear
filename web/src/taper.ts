// A fader's travel and a live decimal's value, pure (docs/editor.md#faders-and-knobs): the row's taper maps travel t
// (0 at its min, 1 at its max) to a value and back, and the words a control shows and a screen reader reads
// ("-3.5 dB", "-inf dB"). ear never sees travel: the wire carries the row's own unit.
import type { SettingSchema } from "./protocol";

/** What the mapping reads of a row: its range, taper, unit, places and whether its min is silence. */
export type TaperRow = Pick<SettingSchema, "min" | "max" | "taper" | "unit" | "decimals" | "minIsSilence">;

/** The db taper's points between its ends, (travel, dB): a mixing desk's fader, 0 dB at three quarters. */
export const dbPoints: readonly (readonly [number, number])[] = [
  [0.1, -60],
  [0.25, -40],
  [0.45, -20],
  [0.6, -10],
  [0.75, 0],
];

/** The taper a row is mapped by: db only when its range holds every point; log only above zero; else linear. */
export function taperOf(row: TaperRow): "linear" | "log" | "db" {
  const { min, max } = bounds(row);
  if (row.taper === "db" && min < -60 && max > 0) return "db";
  if (row.taper === "log" && min > 0 && max > min) return "log";
  return "linear";
}

function bounds(row: TaperRow): { min: number; max: number } {
  return { min: row.min ?? 0, max: row.max ?? 1 };
}

/** The db taper's points with its ends: (0, min) and (1, max). */
function dbCurve(min: number, max: number): (readonly [number, number])[] {
  return [[0, min], ...dbPoints, [1, max]];
}

const clamp01 = (t: number) => (t < 0 ? 0 : t > 1 ? 1 : t);

/** The value at travel `t` (clamped to 0..1). */
export function valueAt(row: TaperRow, t: number): number {
  const travel = clamp01(t);
  const { min, max } = bounds(row);
  switch (taperOf(row)) {
    case "log":
      return min * (max / min) ** travel;
    case "db": {
      const curve = dbCurve(min, max);
      for (let i = 1; i < curve.length; ++i) {
        const [t1, v1] = curve[i]!;
        const [t0, v0] = curve[i - 1]!;
        if (travel <= t1) return v0 + ((travel - t0) / (t1 - t0)) * (v1 - v0);
      }
      return max;
    }
    default:
      return min + travel * (max - min);
  }
}

/** The travel of `value` (clamped to the range): valueAt's inverse. */
export function travelOf(row: TaperRow, value: number): number {
  const { min, max } = bounds(row);
  if (max <= min) return 0;
  const v = value < min ? min : value > max ? max : value;
  switch (taperOf(row)) {
    case "log":
      return Math.log(v / min) / Math.log(max / min);
    case "db": {
      const curve = dbCurve(min, max);
      for (let i = 1; i < curve.length; ++i) {
        const [t1, v1] = curve[i]!;
        const [t0, v0] = curve[i - 1]!;
        if (v <= v1) return t0 + ((v - v0) / (v1 - v0)) * (t1 - t0);
      }
      return 1;
    }
    default:
      return (v - min) / (max - min);
  }
}

/** `value` rounded to the row's places (as ear rounds an end: half away from zero), clamped to its range. */
export function roundedValue(row: TaperRow, value: number): number {
  const { min, max } = bounds(row);
  const places = row.decimals ?? 2;
  const scale = 10 ** places;
  const rounded = (Math.sign(value) * Math.round(Math.abs(value) * scale)) / scale;
  const clamped = rounded < min ? min : rounded > max ? max : rounded;
  return Object.is(clamped, -0) ? 0 : clamped;
}

/** The value a control shows: "-3.5 dB", "-inf dB" at a silent min, with the row's places. */
export function valueText(row: TaperRow, value: number): string {
  const unit = row.unit !== undefined ? ` ${row.unit}` : "";
  if (row.minIsSilence === true && row.min !== undefined && value <= row.min) return `-inf${unit}`;
  return `${roundedValue(row, value).toFixed(row.decimals ?? 2)}${unit}`;
}

/** One keyboard step's travel: 1% (Shift 0.1%), Page Up and Down 10%. */
export function stepTravel(key: string, shift: boolean): number | undefined {
  switch (key) {
    case "ArrowUp":
    case "ArrowRight":
      return shift ? 0.001 : 0.01;
    case "ArrowDown":
    case "ArrowLeft":
      return shift ? -0.001 : -0.01;
    case "PageUp":
      return 0.1;
    case "PageDown":
      return -0.1;
    default:
      return undefined;
  }
}

/** The value after a key on a control showing `value`: a step's travel, Home the min, End the max; else undefined. */
export function valueAfterKey(row: TaperRow, value: number, key: string, shift: boolean): number | undefined {
  const { min, max } = bounds(row);
  if (key === "Home") return min;
  if (key === "End") return max;
  const step = stepTravel(key, shift);
  if (step === undefined) return undefined;
  return valueAt(row, travelOf(row, value) + step);
}
