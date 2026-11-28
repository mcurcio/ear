// The connection rules between the canvas (@xyflow/react) and the wire format
// (docs/editor.md): a drag from an output port to an input port is route.add's
// params, unless the graph already holds that route; any other drag is refused before it reaches the server. Each port
// is one React Flow handle whose id says its direction, so the rule needs only the handles and the graph's routes. It
// is patchbay's useConnectHints's rule (connectRule), so the hints on the handles while a wire is drawn, the status
// line, the drop itself and the inspector's "Connect to" are judged by one rule (docs/editor.md#the-node-box), and a
// hint never promises what the drop refuses. Every other refusal is ear's, after the drop.
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

/** The rule's reason for a route the graph already holds: the status line's words over that port. */
export const alreadyConnected = "Already connected";

/**
 * ear's rule as useConnectHints takes it, over the routes the graph holds: `from` is the drag's source handle, `to` its
 * target, as React Flow reports. A drag between two outputs or two inputs is refused (false), one that would ask for a
 * route already held is refused with alreadyConnected, and any other is accepted.
 */
export function connectRule(routes: readonly RouteEnds[]): CanConnect {
  const held = new Set(routes.map(edgeId));
  return (from, to) => {
    const route = routeFromConnection({
      source: from.nodeId,
      sourceHandle: from.handleId,
      target: to.nodeId,
      targetHandle: to.handleId,
    });
    if (route === null) return false;
    return held.has(edgeId(route)) ? alreadyConnected : true;
  };
}

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
 * the graph by connectRule over the graph's routes (the drag's and the drop's own rule, which refuses a route already
 * held), in the graph's order.
 */
export function portTargets(graph: Graph, node: string, direction: Direction, port: string): PortTarget[] {
  const candidates: Candidate[] = [];
  for (const n of graph.nodes) {
    for (const p of n.inputs) candidates.push({ nodeId: n.name, handleId: handleId("in", p), side: "input", node: n.name, port: p });
    for (const p of n.outputs)
      candidates.push({ nodeId: n.name, handleId: handleId("out", p), side: "output", node: n.name, port: p });
  }
  const self = { nodeId: node, handleId: handleId(direction, port) };
  const side = direction === "in" ? "input" : "output";
  const { accepted } = connectTargets(self, side, candidates, connectRule(graph.routes));
  const here: PortRef = { node, port };
  return accepted.map((target) => {
    const there: PortRef = { node: target.node, port: target.port };
    const ends: RouteEnds = direction === "in" ? { from: there, to: here } : { from: here, to: there };
    return { key: edgeId(ends), label: `${target.node}:${target.port}`, ends };
  });
}
