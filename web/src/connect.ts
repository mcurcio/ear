// The connection rules between the canvas (@xyflow/react) and the wire format
// (docs/editor.md): a drag from an output port to an input port is route.add's
// params; any other drag is refused before it reaches the server. Each port is one React Flow
// handle whose id says its direction, so the rule needs nothing but the handles. It is patchbay's useConnectHints's
// rule (canConnect), so the hints on the handles while a wire is drawn and the drop itself are judged by one rule
// (docs/editor.md#the-node-box), and a hint never promises what the drop refuses.
import { connectTargets, type CanConnect, type ConnectCandidate, type PortSide } from "@mcurcio/patchbay/graph";
import type { Graph, PortRef, RouteEnds } from "./protocol";

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

/** ear's rule as useConnectHints takes it: `from` is the drag's source handle, `to` its target, as React Flow reports. */
export const canConnect: CanConnect = (from, to) =>
  isValidConnection({ source: from.nodeId, sourceHandle: from.handleId, target: to.nodeId, targetHandle: to.handleId });

/**
 * Which quick add patchbay's useAddAt asked for (docs/editor.md#adding-on-the-canvas): `pane` for a double-click on
 * empty canvas (no `from`), `wire` for a wire from an output dropped there, and none for a wire from an input, as quick
 * add follows an output's wire.
 */
export function quickAddKind(from?: { side: PortSide }): "pane" | "wire" | null {
  if (from === undefined) return "pane";
  return from.side === "output" ? "wire" : null;
}

/** One choice of the inspector's "Connect to" (docs/editor.md#the-inspector): the route it asks for, named by its far end. */
export interface PortTarget {
  /** The route's edge id, unique in the list: the option's value. */
  key: string;
  /** The far end as `node:port`. */
  label: string;
  ends: RouteEnds;
}

type Candidate = ConnectCandidate & { node: string; port: string };

/**
 * The ports one port may be joined to, for the inspector's "Connect to": patchbay's connectTargets over every port in
 * the graph by canConnect (the drag's and the drop's own rule), in the graph's order, less the far ends of the routes
 * the graph already holds from or to it, which a choice would only ask for again.
 */
export function portTargets(graph: Graph, node: string, direction: Direction, port: string): PortTarget[] {
  const candidates: Candidate[] = [];
  for (const n of graph.nodes) {
    for (const p of n.inputs) candidates.push({ nodeId: n.name, handleId: handleId("in", p), side: "input", node: n.name, port: p });
    for (const p of n.outputs)
      candidates.push({ nodeId: n.name, handleId: handleId("out", p), side: "output", node: n.name, port: p });
  }
  const self = { nodeId: node, handleId: handleId(direction, port) };
  const { accepted } = connectTargets(self, direction === "in" ? "input" : "output", candidates, canConnect);
  const held = new Set(graph.routes.map(edgeId));
  const here: PortRef = { node, port };
  const targets: PortTarget[] = [];
  for (const target of accepted) {
    const there: PortRef = { node: target.node, port: target.port };
    const ends: RouteEnds = direction === "in" ? { from: there, to: here } : { from: here, to: there };
    if (held.has(edgeId(ends))) continue;
    targets.push({ key: edgeId(ends), label: `${target.node}:${target.port}`, ends });
  }
  return targets;
}
