// The connection rules between the canvas (@xyflow/react) and the wire format
// (docs/design/batch2.md section 8): a drag from an output port to an input port is route.add's
// params; any other drag is refused before it reaches the server. Each port is one React Flow
// handle whose id says its direction, so the rule needs nothing but the handles.
import type { PortRef, RouteEnds } from "./protocol";

export type Direction = "in" | "out";

/** The handle id of a node's port: its direction and its name. */
export function handleId(direction: Direction, port: string): string {
  return `${direction}:${port}`;
}

export function parseHandleId(id: string | null | undefined): { direction: Direction; port: string } | null {
  if (id === null || id === undefined) return null;
  const colon = id.indexOf(":");
  const direction = id.slice(0, colon);
  if (colon < 0 || (direction !== "in" && direction !== "out")) return null;
  return { direction, port: id.slice(colon + 1) };
}

/** What React Flow reports for a drag between two handles (its Connection, or an Edge). */
export interface HandlePair {
  source: string;
  sourceHandle?: string | null;
  target: string;
  targetHandle?: string | null;
}

/**
 * route.add's (and route.remove's) params for a drag between two ports: from the output, to the
 * input, whichever end the drag started at. null when both ends are outputs or both inputs.
 */
export function routeFromConnection(connection: HandlePair): RouteEnds | null {
  const source = parseHandleId(connection.sourceHandle);
  const target = parseHandleId(connection.targetHandle);
  if (source === null || target === null || source.direction === target.direction) return null;
  const sourcePort: PortRef = { node: connection.source, port: source.port };
  const targetPort: PortRef = { node: connection.target, port: target.port };
  return source.direction === "out" ? { from: sourcePort, to: targetPort } : { from: targetPort, to: sourcePort };
}

export function isValidConnection(connection: HandlePair): boolean {
  return routeFromConnection(connection) !== null;
}

/** The canvas id of a route's wire: one per route, the same for the same ends. */
export function edgeId(route: RouteEnds): string {
  return JSON.stringify([route.from.node, route.from.port, route.to.node, route.to.port]);
}
