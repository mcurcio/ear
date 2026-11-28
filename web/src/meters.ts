// A meter's words (docs/editor.md#meters): the tooltip patchbay's Meter shows for a port, in dBFS, "silent" at or below
// its floor and "clipped" while its lamp is lit, its numbers written by patchbay's formatMeterDb as Meter's own tooltip
// writes them. The scale, the zones, the hold, the fall and the lamp are patchbay's Meter's, at its defaults (the owner
// chose that patchbay's Meter owns the dB scale).
import { defaultMeterScale, formatMeterDb, type MeterTitle } from "@mcurcio/patchbay/graph";

/** "peak -6.0 dBFS, rms -12.0 dBFS", "; clipped" while the lamp is lit; "silent" or "clipped" with nothing above the floor. */
export const meterTitle: MeterTitle = (peakDb, rmsDb, clipped) => {
  const { floor } = defaultMeterScale;
  if (!(peakDb > floor)) return clipped ? "clipped" : "silent";
  const rms = Math.max(floor, rmsDb);
  return `peak ${formatMeterDb(peakDb)} dBFS, rms ${formatMeterDb(rms)} dBFS${clipped ? "; clipped" : ""}`;
};
