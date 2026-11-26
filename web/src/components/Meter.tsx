// One port's meter (docs/editor.md#meters): patchbay's Meter fed by the level store, which owns the dBFS scale, the
// zones, the peak tick's hold and fall and the clip lamp, and paints through refs, so neither the levels nor the hold
// are React state (docs/editor.md#its-rules). ear gives it the port's level, its words (meters.ts) and a reset when the
// connection closes.
import { Meter, type MeterHandle } from "@mcurcio/patchbay/graph";
import { useCallback, useEffect, useRef } from "react";
import { meterTitle } from "../meters";
import { useConnection, useEditor } from "./hooks";

export function PortMeter({ node, port, label }: { node: string; port: string; label: string }) {
  const { levels } = useEditor();
  const status = useConnection();
  const meter = useRef<MeterHandle>(null);
  const subscribe = useCallback((listener: () => void) => levels.subscribe(node, port, listener), [levels, node, port]);
  const read = useCallback((): readonly [number, number] => {
    const [peak, rms] = levels.get(node, port);
    return [peak, rms];
  }, [levels, node, port]);

  // The Editor clears the levels before it tells the status listeners; the reset also forgets the hold and the lamp.
  useEffect(() => {
    if (status === "closed") meter.current?.reset();
  }, [status]);

  return <Meter ref={meter} className="port-meter" subscribe={subscribe} read={read} label={label} title={meterTitle} />;
}
