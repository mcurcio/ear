// The connection rules between the canvas (@xyflow/react) and the wire format
// (docs/editor.md): a drag from an output port to an input port is route.add's
// params; any other drag is refused before it reaches the server. Each port is one React Flow
// handle whose id says its direction, so the rule needs nothing but the handles. While a wire is drawn, the ring on the
// handles it can be dropped on follows the same rule (handleCue, docs/editor.md#the-node-box), so the ring never
// promises what the drop refuses.
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

/** The end a drag in progress started from: React Flow's fromHandle (its node and handle id). */
export interface DragStart {
  nodeId: string;
  id?: string | null;
}

export type HandleCue = "idle" | "origin" | "target" | "dimmed";

/**
 * How a handle is drawn while a wire is drawn from `from`: idle with no drag; origin for the handle the drag started
 * at; target for every handle a drop on would make a route (isValidConnection: from an output every input, from an
 * input every output); dimmed for every other.
 */
export function handleCue(from: DragStart | null, node: string, handle: string): HandleCue {
  if (from === null) return "idle";
  if (from.nodeId === node && from.id === handle) return "origin";
  return isValidConnection({ source: from.nodeId, sourceHandle: from.id, target: node, targetHandle: handle })
    ? "target"
    : "dimmed";
}
