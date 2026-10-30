// One port's level meter: a bar for the rms and a tick for the peak, in dBFS down to -60.
import { floorDb, meterFraction, toDbfs } from "../levels";
import { useLevel } from "./hooks";

export function Meter({ node, port }: { node: string; port: string }) {
  const [peak, rms] = useLevel(node, port);
  const db = toDbfs(peak);
  return (
    <div className="meter" title={db <= floorDb ? "silent" : `peak ${db.toFixed(1)} dBFS, rms ${toDbfs(rms).toFixed(1)} dBFS`}>
      <div className="meter-rms" style={{ width: `${meterFraction(rms) * 100}%` }} />
      <div className="meter-peak" style={{ left: `${meterFraction(peak) * 100}%` }} />
    </div>
  );
}
