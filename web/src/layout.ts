// First placement of nodes on the canvas (docs/editor.md), with
// @dagrejs/dagre, the layout React Flow's docs use: sources on the left, sinks on the right,
// following the routes. Only nodes without a place get one; a place the user dragged a node to, or where quick add put
// it, is kept in the browser through prefs.ts (docs/editor.md#what-the-browser-keeps), never sent to ear.
import { Graph as LayoutGraph, layout } from "@dagrejs/dagre";
import type { Graph, GraphNode } from "./protocol";

export interface Position {
  x: number;
  y: number;
}

/** A box's width: editor.css's .ear-node width, which layout.test.ts holds equal (docs/editor.md#the-node-box). */
export const nodeWidth = 220;
/**
 * The header's three rows (the title 18, the state pill 20, the health line 16, its padding 12 and border 1), rounded
 * up: dagre needs only an estimate, while the width is exact.
 */
export const headerHeight = 68;
/** A port row's height: editor.css's .port height, which layout.test.ts holds equal. */
export const portHeight = 22;

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

/** Where places outlive the page: prefs.ts's, over the browser's storage; it never throws. */
export interface PlaceKeeper {
  load(): ReadonlyMap<string, Position>;
  /** `present` is every node's place now; `forgetAbsent` (Tidy) forgets the rest. */
  save(present: ReadonlyMap<string, Position>, forgetAbsent: boolean): void;
}

/**
 * Where quick add asked for a node (docs/editor.md#adding-on-the-canvas): `name` is the template's, or null (the JSON
 * form, which has no template) for the next new node that arrives.
 */
export interface PlaceHint {
  name: string | null;
  position: Position;
}

/** A key of the present places, so a replay of state changes writes nothing. */
function placesKey(positions: ReadonlyMap<string, Position>): string {
  return JSON.stringify([...positions].sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0)));
}

/**
 * Where each node name was last placed, loaded from and saved to its keeper (docs/editor.md#what-the-browser-keeps).
 * A node removed and added again under the same name keeps its place: that is how ear replaces a node by another type
 * (node.removed, then node.added, from an edit at the source), and the canvas may draw the graph in between.
 */
export class Places {
  private readonly known: Map<string, Position>;
  private pending: PlaceHint | null = null;
  private saved = "";

  constructor(private readonly keeper?: PlaceKeeper) {
    this.known = new Map(keeper?.load() ?? []);
  }

  /**
   * Every node's position in `graph`: where the canvas has it now (`current`, moves included), else a placement
   * hint's, else where it was last placed, else a new layout.
   */
  place(graph: Graph, current: ReadonlyMap<string, Position>): Map<string, Position> {
    for (const [name, position] of current) this.known.set(name, position);
    this.takeHint(graph, current);
    const positions = placeNodes(graph, this.known);
    for (const [name, position] of positions) this.known.set(name, position);
    this.save(positions, false);
    return positions;
  }

  /** A hint for an arriving node beats its kept place; one whose node arrived under another name is dropped. */
  private takeHint(graph: Graph, current: ReadonlyMap<string, Position>): void {
    const hint = this.pending;
    if (hint === null) return;
    const arriving = graph.nodes.map((node) => node.name).filter((name) => !current.has(name));
    const fresh = arriving.filter((name) => !this.known.has(name));
    const target = hint.name === null ? fresh[0] : arriving.includes(hint.name) ? hint.name : undefined;
    if (target !== undefined) {
      this.known.set(target, hint.position);
      this.pending = null;
    } else if (fresh.length > 0) {
      // A new node under another name: the person renamed the template, so it is laid out as any other.
      this.pending = null;
    }
  }

  /** A drag ended (or a keyboard move): those places are known and kept. */
  move(moved: ReadonlyMap<string, Position>): void {
    if (moved.size === 0) return;
    for (const [name, position] of moved) this.known.set(name, position);
    this.keeper?.save(moved, false);
  }

  /** Quick add's hint, replacing any held; null drops it. */
  hint(hint: PlaceHint | null): void {
    this.pending = hint;
  }

  /**
   * Tidy (docs/editor.md#the-canvas): every node laid out again as on first placement, every known place forgotten,
   * the kept ones too; moves are the browser's, nothing is sent.
   */
  relayout(graph: Graph): Map<string, Position> {
    this.known.clear();
    const positions = placeNodes(graph, new Map());
    for (const [name, position] of positions) this.known.set(name, position);
    this.save(positions, true);
    return positions;
  }

  private save(present: ReadonlyMap<string, Position>, forgetAbsent: boolean): void {
    const key = placesKey(present);
    if (!forgetAbsent && key === this.saved) return;
    this.saved = key;
    this.keeper?.save(present, forgetAbsent);
  }
}

/**
 * Where quick add puts a node (docs/editor.md#adding-on-the-canvas), at `point` on the flow: from a double-click on
 * the pane the box centred on it with its header there; from an output's wire dropped there the box's left edge (its
 * inputs) at the point, its first port row at the point's height.
 */
export function quickPlace(point: Position, from: "pane" | "wire"): Position {
  if (from === "pane") return { x: point.x - nodeWidth / 2, y: point.y - headerHeight / 2 };
  return { x: point.x, y: point.y - headerHeight - portHeight / 2 };
}
