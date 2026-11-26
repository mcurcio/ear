// A meter's words (docs/editor.md#meters): the tooltip patchbay's Meter shows for a port, in dBFS, "silent" at or below
// its floor and "clipped" while its lamp is lit. The scale, the zones, the hold, the fall and the lamp are patchbay's
// Meter's, at its defaults (the owner chose that patchbay's Meter owns the dB scale, D10).
import { defaultMeterScale, type MeterTitle } from "@mcurcio/patchbay/graph";

/** "peak -6.0 dBFS, rms -12.0 dBFS", "; clipped" while the lamp is lit; "silent" or "clipped" with nothing above the floor. */
export const meterTitle: MeterTitle = (peakDb, rmsDb, clipped) => {
  const { floor } = defaultMeterScale;
  if (!(peakDb > floor)) return clipped ? "clipped" : "silent";
  const rms = Math.max(floor, rmsDb);
  return `peak ${peakDb.toFixed(1)} dBFS, rms ${rms.toFixed(1)} dBFS${clipped ? "; clipped" : ""}`;
};
