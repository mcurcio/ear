// What the canvas has selected and what the inspector shows for it, pure (docs/editor.md#the-inspector): the subject
// (the graph, one node, one route or several), the count in words, and the requests Delete makes. Names and wire ids
// are resolved against the graph ear announced, so the inspector never shows what ear has not.
import { edgeId } from "./connect";
import type { Graph, GraphNode, GraphRoute, RouteEnds } from "./protocol";

/** What the canvas has selected: node names and wire ids (connect.ts's edgeId). */
export interface Selection {
  nodes: readonly string[];
  routes: readonly string[];
}

export const nothingSelected: Selection = { nodes: [], routes: [] };

/** A stable key for effects and memos: the names and ids, sorted, joined; "" for nothing selected. */
export function selectionKey(selection: Selection): string {
  if (selection.nodes.length === 0 && selection.routes.length === 0) return "";
  return JSON.stringify([[...selection.nodes].sort(), [...selection.routes].sort()]);
}

/** The selection from React Flow's nodes (their selected flag) and the selected wire ids, in canvas order. */
export function selectionOf(
  nodes: readonly { id: string; selected?: boolean }[],
  routes: ReadonlySet<string>,
): Selection {
  return { nodes: nodes.filter((node) => node.selected === true).map((node) => node.id), routes: [...routes] };
}

/** The nodes with `selected` set exactly for `names`; a node whose flag does not change keeps its object. */
export function withSelected<T extends { id: string; selected?: boolean }>(
  nodes: readonly T[],
  names: ReadonlySet<string>,
): T[] {
  return nodes.map((node) => {
    const selected = names.has(node.id);
    return (node.selected === true) === selected ? node : { ...node, selected };
  });
}

export type InspectorSubject =
  | { kind: "graph" }
  | { kind: "node"; node: GraphNode }
  | { kind: "route"; route: GraphRoute }
  | { kind: "several"; nodes: GraphNode[]; routes: GraphRoute[]; text: string };

/**
 * The inspector's subject: nothing is the graph, one node its Node, one route its Route, anything else several (in the
 * graph's order). A name or wire id the graph does not hold is ignored.
 */
export function subjectOf(graph: Graph, selection: Selection): InspectorSubject {
  const names = new Set(selection.nodes);
  const ids = new Set(selection.routes);
  const nodes = graph.nodes.filter((node) => names.has(node.name));
  const routes = graph.routes.filter((route) => ids.has(edgeId(route)));
  if (nodes.length === 0 && routes.length === 0) return { kind: "graph" };
  if (nodes.length === 1 && routes.length === 0) return { kind: "node", node: nodes[0]! };
  if (nodes.length === 0 && routes.length === 1) return { kind: "route", route: routes[0]! };
  return { kind: "several", nodes, routes, text: selectionText(nodes.length, routes.length) };
}

/** The inspector's heading: "Graph", the node's name, "Route", or the count. */
export function subjectTitle(subject: InspectorSubject): string {
  switch (subject.kind) {
    case "graph":
      return "Graph";
    case "node":
      return subject.node.name;
    case "route":
      return "Route";
    case "several":
      return subject.text;
  }
}

function counted(count: number, one: string, many: string): string {
  return `${count} ${count === 1 ? one : many}`;
}

/** "1 node", "2 nodes", "1 route", "3 nodes, 2 routes"; nothing selected gives "". */
export function selectionText(nodes: number, routes: number): string {
  const parts: string[] = [];
  if (nodes > 0) parts.push(counted(nodes, "node", "nodes"));
  if (routes > 0) parts.push(counted(routes, "route", "routes"));
  return parts.join(", ");
}

/**
 * The requests Delete makes: node.remove for each node, and route.remove only for routes between kept nodes, since
 * removing a node removes its routes.
 */
export function removals(
  nodes: readonly string[],
  routes: readonly RouteEnds[],
): { nodes: string[]; routes: RouteEnds[] } {
  const names = [...new Set(nodes)];
  const doomed = new Set(names);
  return {
    nodes: names,
    routes: routes.filter((route) => !doomed.has(route.from.node) && !doomed.has(route.to.node)),
  };
}
