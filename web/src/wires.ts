// What a route's wire draws (docs/editor.md#wires): its status's colour class, a thickness and brightness by its from
// port's level, a flow while a linked wire carries signal, its status in words where colour is not enough, and a
// tooltip naming its two ends; and the inspector's Route subject's status in words (wireStatusWords). A status this
// editor does not know is drawn as pending, neutrally, and by its own word
// (docs/control-port.md#protocol-and-features).
import { floorDb, meterFraction, toDbfs } from "./levels";
import type { GraphRoute, RouteEnds } from "./protocol";

export type WireStatusClass = "wire-linked" | "wire-pending" | "wire-refused" | "wire-unknown";

export interface WireView {
  statusClass: WireStatusClass;
  /** Stroke width in px: 1.5 + 5 at full scale, + 1.5 when selected (hover adds 1 px in editor.css). */
  width: number;
  /** 0.45 + 0.55 at full scale. */
  opacity: number;
  /** Every status but linked is dashed and still. */
  dashed: boolean;
  /** A linked wire whose from port's rms is above the floor shows the flow; no other status ever does. */
  flowing: boolean;
  /** The status in words at the midpoint: "refused", an unknown status's own word, or none. */
  label: string | undefined;
  /** The hover tooltip: the two ends, and a refused route's detail on a second line. */
  title: string;
}

const statusClasses: Record<string, WireStatusClass> = {
  linked: "wire-linked",
  pending: "wire-pending",
  refused: "wire-refused",
};

function statusClassOf(route: GraphRoute): WireStatusClass {
  const status: string = route.status;
  return Object.hasOwn(statusClasses, status) ? statusClasses[status]! : "wire-unknown";
}

/** The Route subject's status in words (docs/editor.md#the-inspector): an unknown status by its own word, as sent. */
export function wireStatusWords(route: GraphRoute): string {
  switch (statusClassOf(route)) {
    case "wire-linked":
      return "linked";
    case "wire-pending":
      return "pending: ear has not linked it yet";
    case "wire-refused":
      return route.detail !== undefined ? `refused: ${route.detail}` : "refused";
    case "wire-unknown":
      return route.status;
  }
}

/** A wire's two ends as text: `noise:noise.mono → rec:record.right`. */
export function wireEnds(route: RouteEnds): string {
  return `${route.from.node}:${route.from.port} → ${route.to.node}:${route.to.port}`;
}

/** What the wire of `route` draws while its from port's rms is `rms`. */
export function wireView(route: GraphRoute, rms: number, selected: boolean): WireView {
  const status: string = route.status;
  const statusClass = statusClassOf(route);
  const level = meterFraction(rms);
  const linked = statusClass === "wire-linked";
  let label: string | undefined;
  if (statusClass === "wire-refused") label = "refused";
  else if (statusClass === "wire-unknown") label = status;
  const ends = wireEnds(route);
  return {
    statusClass,
    width: 1.5 + 5 * level + (selected ? 1.5 : 0),
    opacity: 0.45 + 0.55 * level,
    dashed: !linked,
    flowing: linked && toDbfs(rms) > floorDb,
    label,
    title: statusClass === "wire-refused" && route.detail !== undefined ? `${ends}\nrefused: ${route.detail}` : ends,
  };
}
