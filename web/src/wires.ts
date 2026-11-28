// What a route's wire draws (docs/editor.md#wires), as patchbay's Wire takes it: its status's tone, an emphasis by its
// from port's level, dashes on every status but linked, a flow while a linked wire carries signal, its status in words
// where colour is not enough, and a tooltip naming its two ends; and the inspector's Route subject's status in words
// (wireStatusWords) and class (wireStatusClass). A status this editor does not know is drawn as pending, and by its
// own word (docs/control-port.md#protocol-and-features).
import type { WireData, WireTone } from "@mcurcio/patchbay/graph";
import { floorDb, meterFraction, toDbfs } from "./levels";
import type { GraphRoute, RouteEnds } from "./protocol";

export type WireStatusClass = "wire-linked" | "wire-pending" | "wire-refused" | "wire-unknown";

const statusClasses: Record<string, WireStatusClass> = {
  linked: "wire-linked",
  pending: "wire-pending",
  refused: "wire-refused",
};

/** The status's class: the Route subject colours its status words by it. */
export function wireStatusClass(route: GraphRoute): WireStatusClass {
  const status: string = route.status;
  return Object.hasOwn(statusClasses, status) ? statusClasses[status]! : "wire-unknown";
}

/** The Route subject's status in words (docs/editor.md#the-inspector): an unknown status by its own word, as sent. */
export function wireStatusWords(route: GraphRoute): string {
  switch (wireStatusClass(route)) {
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

/**
 * Each status's tone on Wire: linked ok, refused refused, and pending (ear holds the route and has not linked it; not a
 * request on its way, which is a GhostWire) the quiet tone, dashed; an unknown status as pending's.
 */
const tones: Record<WireStatusClass, WireTone> = {
  "wire-linked": "ok",
  "wire-pending": "quiet",
  "wire-unknown": "quiet",
  "wire-refused": "refused",
};

/**
 * What the wire of `route` draws while its from port's rms is `rms`: Wire's data. The emphasis is the meter's fraction
 * of the level, which Wire takes as a CSS variable, so a new level restyles the wire without rebuilding its path.
 */
export function wireData(route: GraphRoute, rms: number): WireData {
  const status: string = route.status;
  const statusClass = wireStatusClass(route);
  const linked = statusClass === "wire-linked";
  let label: string | undefined;
  if (statusClass === "wire-refused") label = "refused";
  else if (statusClass === "wire-unknown") label = status;
  const ends = wireEnds(route);
  return {
    tone: tones[statusClass],
    emphasis: meterFraction(rms),
    dashed: !linked,
    flow: linked && toDbfs(rms) > floorDb,
    label,
    title: statusClass === "wire-refused" && route.detail !== undefined ? `${ends}\nrefused: ${route.detail}` : ends,
  };
}
