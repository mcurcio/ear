// One port's meter (docs/editor.md#meters): the rms bar over three zones, the peak tick's hold and fall, the clip lamp.
// It is painted from the level store through refs, so neither the levels nor the hold are React state
// (docs/editor.md#its-rules), and it animates only while the tick falls or the lamp is lit. React renders the
// structure once and sets nothing the paint sets, so a render never overwrites it.
import { useEffect, useLayoutEffect, useRef, type CSSProperties } from "react";
import { PeakHold, describeMeter, zoneStops } from "../meters";
import { useConnection, useEditor } from "./hooks";

const zones = {
  "--meter-mid-at": `${zoneStops.mid * 100}%`,
  "--meter-hot-at": `${zoneStops.hot * 100}%`,
} as CSSProperties;

export function Meter({ node, port }: { node: string; port: string }) {
  const { levels } = useEditor();
  const status = useConnection();
  const meter = useRef<HTMLDivElement>(null);
  const hold = useRef<PeakHold | null>(null);
  hold.current ??= new PeakHold();
  const frame = useRef<number | undefined>(undefined);
  const paint = useRef<(now: number) => void>(() => {});

  paint.current = (now: number) => {
    const element = meter.current;
    if (element === null) return;
    const view = describeMeter(levels.get(node, port), hold.current!, now);
    element.style.setProperty("--meter-rms-at", String(view.rms));
    element.style.setProperty("--meter-peak-at", String(view.peak));
    element.toggleAttribute("data-clipped", view.clipped);
    element.title = view.title;
  };

  useLayoutEffect(() => {
    const peakHold = hold.current!;
    const stop = () => {
      if (frame.current !== undefined) cancelAnimationFrame(frame.current);
      frame.current = undefined;
    };
    // One loop at most: a frame already asked for carries on.
    const animate = () => {
      if (frame.current !== undefined || !peakHold.moving(performance.now())) return;
      const step = () => {
        frame.current = undefined;
        const now = performance.now();
        paint.current(now);
        if (peakHold.moving(now)) frame.current = requestAnimationFrame(step);
      };
      frame.current = requestAnimationFrame(step);
    };
    const take = () => {
      const now = performance.now();
      peakHold.take(levels.get(node, port)[0], now);
      paint.current(now);
      animate();
    };
    take();
    const unsubscribe = levels.subscribe(node, port, take);
    return () => {
      unsubscribe();
      stop();
    };
  }, [levels, node, port]);

  // The Editor clears the levels before it tells the status listeners, so this paints silence with no hold.
  useEffect(() => {
    if (status !== "closed") return;
    hold.current!.reset();
    if (frame.current !== undefined) cancelAnimationFrame(frame.current);
    frame.current = undefined;
    paint.current(performance.now());
  }, [status]);

  return (
    <div className="meter" ref={meter} style={zones}>
      <div className="meter-rms" />
      <div className="meter-peak" />
      <div className="meter-clip" aria-hidden="true" />
    </div>
  );
}
