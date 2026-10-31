// First placement of nodes on the canvas (docs/design/batch2.md section 8), with
// @dagrejs/dagre, the layout React Flow's docs use: sources on the left, sinks on the right,
// following the routes. Only nodes without a place get one; a place the user dragged a node to
// is kept (in the browser only, never sent to ear).
import { Graph as LayoutGraph, layout } from "@dagrejs/dagre";
import type { Graph, GraphNode } from "./protocol";

export interface Position {
  x: number;
  y: number;
}

export const nodeWidth = 200;
const headerHeight = 56;
const portHeight = 22;

/** A node box's size on the canvas: one row per port on its longer side. */
export function nodeSize(node: GraphNode): { width: number; height: number } {
  return { width: nodeWidth, height: headerHeight + portHeight * Math.max(1, node.inputs.length, node.outputs.length) };
}

/**
 * Every node's top-left position: `placed` for the nodes it has, and a dagre layout of the whole
 * graph for the rest.
 */
export function placeNodes(graph: Graph, placed: ReadonlyMap<string, Position>): Map<string, Position> {
  const positions = new Map<string, Position>();
  const unplaced = graph.nodes.filter((node) => !placed.has(node.name));
  for (const node of graph.nodes) {
    const position = placed.get(node.name);
    if (position !== undefined) positions.set(node.name, position);
  }
  if (unplaced.length === 0) return positions;

  const g = new LayoutGraph();
  g.setGraph({ rankdir: "LR", nodesep: 40, ranksep: 120, marginx: 20, marginy: 20 });
  g.setDefaultEdgeLabel(() => ({}));
  for (const node of graph.nodes) g.setNode(node.name, nodeSize(node));
  for (const route of graph.routes) g.setEdge(route.from.node, route.to.node);
  layout(g);
  for (const node of unplaced) {
    const { x, y, width, height } = g.node(node.name) as { x: number; y: number; width: number; height: number };
    // dagre gives centres; the canvas places nodes by their top-left corner.
    positions.set(node.name, { x: x - width / 2, y: y - height / 2 });
  }
  return positions;
}

/**
 * Where each node name was last placed, for the page's life. A node removed and added again under
 * the same name keeps its place: that is how ear replaces a node whose ports change (node.removed,
 * then node.added, from an edit at the source), and the canvas may draw the graph in between.
 */
export class Places {
  private readonly known = new Map<string, Position>();

  /**
   * Every node's position in `graph`: where the canvas has it now (`current`, moves included),
   * else where it was last placed, else a new layout.
   */
  place(graph: Graph, current: ReadonlyMap<string, Position>): Map<string, Position> {
    for (const [name, position] of current) this.known.set(name, position);
    const positions = placeNodes(graph, this.known);
    for (const [name, position] of positions) this.known.set(name, position);
    return positions;
  }
}
