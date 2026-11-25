// One port's meter as drawn (docs/editor.md#meters): the rms bar over three fixed zones, the peak tick held and then
// falling, and the clip lamp. The hold lives in a PeakHold the meter keeps outside React state
// (docs/editor.md#its-rules), and is reset when the connection closes. Times are the browser's monotonic clock.
import { floorDb, fractionOfDb, meterFraction, toDbfs } from "./levels";
import type { Level } from "./protocol";

/** --meter-low below this, --meter-mid from here. */
export const midZoneDb = -18;
/** --meter-hot above this. */
export const hotZoneDb = -6;
/** How long the tick holds its highest peak before it falls. */
export const peakHoldMs = 1500;
/** How fast the tick falls once the hold is over. */
export const peakFallDbPerS = 20;
/** How long a peak at or over full scale keeps the lamp lit. */
export const clipLampMs = 2000;

/** Where the zones start on the bar, 0..1 along it: the same scale as meterFraction. */
export const zoneStops: { mid: number; hot: number } = { mid: fractionOfDb(midZoneDb), hot: fractionOfDb(hotZoneDb) };

/**
 * A meter's peak tick: the highest peak held for peakHoldMs, then falling at peakFallDbPerS, never below the live peak
 * (the last reading) and never below floorDb; and the clip lamp, lit for clipLampMs from the last peak at or over full
 * scale (linear 1). Times are ms on the browser's monotonic clock (performance.now()).
 */
export class PeakHold {
  private heldDb = floorDb;
  private heldAt = -Infinity;
  private liveDb = floorDb;
  private clipAt: number | undefined = undefined;

  /** A new reading of the port's peak (linear) at `now`: a peak at or above the tick holds again from now. */
  take(peakLinear: number, now: number): void {
    const db = toDbfs(peakLinear);
    if (db >= this.tickDb(now)) {
      this.heldDb = db;
      this.heldAt = now;
    }
    this.liveDb = db;
    if (peakLinear >= 1) this.clipAt = now;
  }

  /** The tick in dBFS at `now`. */
  tickDb(now: number): number {
    const since = now - this.heldAt;
    const held = since < peakHoldMs ? this.heldDb : this.heldDb - (peakFallDbPerS * (since - peakHoldMs)) / 1000;
    return Math.max(floorDb, this.liveDb, held);
  }

  /** True while the lamp is lit. */
  clipped(now: number): boolean {
    return this.clipAt !== undefined && now - this.clipAt < clipLampMs;
  }

  /**
   * True while something drawn still changes without a new reading: the tick above the live peak and the floor (held
   * or falling), or the lamp lit. The meter animates only while this holds.
   */
  moving(now: number): boolean {
    const tick = this.tickDb(now);
    return (tick > this.liveDb && tick > floorDb) || this.clipped(now);
  }

  /** Back to silence: no hold, the live peak at the floor, the lamp out (the connection closed). */
  reset(): void {
    this.heldDb = floorDb;
    this.heldAt = -Infinity;
    this.liveDb = floorDb;
    this.clipAt = undefined;
  }
}

export interface MeterView {
  /** The rms bar's length, 0..1. */
  rms: number;
  /** The peak tick's place, 0..1. */
  peak: number;
  clipped: boolean;
  /** The tooltip: the live peak and rms in dBFS, "silent", and "clipped" while the lamp is lit. */
  title: string;
}

/** What the meter draws for this level and hold at `now` (after hold.take for a new level). */
export function describeMeter(level: Level, hold: PeakHold, now: number): MeterView {
  const [peak, rms] = level;
  const clipped = hold.clipped(now);
  const silent = toDbfs(peak) <= floorDb;
  let title: string;
  if (silent) title = clipped ? "clipped" : "silent";
  else title = `peak ${toDbfs(peak).toFixed(1)} dBFS, rms ${toDbfs(rms).toFixed(1)} dBFS${clipped ? "; clipped" : ""}`;
  return { rms: meterFraction(rms), peak: fractionOfDb(hold.tickDb(now)), clipped, title };
}
