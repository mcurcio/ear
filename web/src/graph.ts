// The editor's graph: what the server has announced, and nothing else (docs/design/batch2.md
// section 8). One reducer turns each server text into the next graph; a snapshot (the result of
// graph.get or graph.subscribe) replaces everything, and an event naming a node or route the
// client does not know changes nothing. The UI never draws a node or a wire the server has not
// announced: the server is the one source of truth. node.state replaces every health member (one
// absent is absent now, control-plane.md 4.4); members and values this editor does not know are
// taken as they come or ignored, never a failure (4.1).
import type {
  Graph,
  GraphNode,
  GraphRoute,
  NodeHealth,
  Notifications,
  PortRef,
  RouteEnds,
  ServerMessage,
} from "./protocol";

/** The health members a node.state carries (control-plane.md 4.8), the ones it replaces. */
const healthKeys = ["state", "phase", "since", "wanted", "blockLost", "cause", "waitingFor", "reason"] as const;

/** The health members `from` has, and only those: a member ear adds later is not this editor's to keep. */
function healthOf(from: Partial<NodeHealth>): NodeHealth {
  const health: Partial<Record<(typeof healthKeys)[number], unknown>> = {};
  for (const key of healthKeys) if (from[key] !== undefined) health[key] = from[key];
  return health as unknown as NodeHealth;
}

/** What a node.added from a protocol 0 ear, which sends no health, is taken to say. */
const protocol0Health: NodeHealth = { state: "starting", phase: "added", wanted: "running", blockLost: false };

/** A node without its health members. */
function withoutHealth(node: GraphNode): Omit<GraphNode, keyof NodeHealth> {
  const { state: _s, phase: _p, since: _t, wanted: _w, blockLost: _b, cause: _c, waitingFor: _f, reason: _r, ...spec } =
    node;
  return spec;
}

/** No graph yet: nothing has been received since the page loaded. */
export type GraphState = Graph | null;

/** True when a result is a graph (graph.get's and graph.subscribe's); other results are not. */
function isGraph(value: unknown): value is Graph {
  return (
    typeof value === "object" &&
    value !== null &&
    Array.isArray((value as Graph).nodes) &&
    Array.isArray((value as Graph).routes)
  );
}

function samePort(a: PortRef, b: PortRef): boolean {
  return a.node === b.node && a.port === b.port;
}

export function sameRoute(a: RouteEnds, b: RouteEnds): boolean {
  return samePort(a.from, b.from) && samePort(a.to, b.to);
}

function compare(a: string, b: string): number {
  return a < b ? -1 : a > b ? 1 : 0;
}

/** Route order (the server's): by from node, from port, to node, to port. */
function compareRoutes(a: RouteEnds, b: RouteEnds): number {
  return (
    compare(a.from.node, b.from.node) ||
    compare(a.from.port, b.from.port) ||
    compare(a.to.node, b.to.node) ||
    compare(a.to.port, b.to.port)
  );
}

function hasNode(graph: Graph, name: string): boolean {
  return graph.nodes.some((node) => node.name === name);
}

function updateNode(graph: Graph, name: string, change: (node: GraphNode) => GraphNode): Graph {
  if (!hasNode(graph, name)) return graph;
  return { ...graph, nodes: graph.nodes.map((node) => (node.name === name ? change(node) : node)) };
}

type Event<N extends keyof Notifications> = Notifications[N];

const handlers: { [N in keyof Notifications]: (graph: Graph, params: Event<N>) => Graph } = {
  "node.added": (graph, added) => {
    const { name, type, settings, inputs, outputs, disabled } = added;
    if (hasNode(graph, name)) return graph;
    const node: GraphNode = {
      name,
      type,
      settings,
      inputs,
      outputs,
      // A protocol 0 ear sends no health: such a node is starting.
      ...healthOf({ ...protocol0Health, ...added }),
      ...(disabled === true ? { disabled } : {}),
    };
    const nodes = [...graph.nodes, node].sort((a, b) => compare(a.name, b.name));
    return { ...graph, nodes };
  },
  "node.updated": (graph, { name, settings, disabled }) =>
    updateNode(graph, name, ({ disabled: _old, ...node }) =>
      disabled === true ? { ...node, settings, disabled } : { ...node, settings },
    ),
  "node.removed": (graph, { name }) =>
    hasNode(graph, name) ? { ...graph, nodes: graph.nodes.filter((node) => node.name !== name) } : graph,
  "node.state": (graph, params) =>
    updateNode(graph, params.name, (node) => {
      const spec = withoutHealth(node);
      return { ...spec, ...healthOf(params) } as GraphNode;
    }),
  // The node is already gone from the graph (node.removed came first); nothing to draw.
  "node.stopped": (graph) => graph,
  "node.gone": (graph) => graph,
  "route.added": (graph, { from, to }) => {
    const route: GraphRoute = { from, to, status: "pending" };
    if (!hasNode(graph, from.node) || !hasNode(graph, to.node)) return graph;
    if (graph.routes.some((known) => sameRoute(known, route))) return graph;
    return { ...graph, routes: [...graph.routes, route].sort(compareRoutes) };
  },
  "route.removed": (graph, ends) =>
    graph.routes.some((known) => sameRoute(known, ends))
      ? { ...graph, routes: graph.routes.filter((known) => !sameRoute(known, ends)) }
      : graph,
  "route.status": (graph, { from, to, status, detail }) => {
    if (!graph.routes.some((known) => sameRoute(known, { from, to }))) return graph;
    const routes = graph.routes.map((known): GraphRoute => {
      if (!sameRoute(known, { from, to })) return known;
      return detail !== undefined ? { from: known.from, to: known.to, status, detail } : { from: known.from, to: known.to, status };
    });
    return { ...graph, routes };
  },
  // Levels live in their own store (levels.ts) and never change the graph.
  levels: (graph) => graph,
  // The file lives in its own store (graphFile.ts); the graph events it causes follow it.
  "graphFile.changed": (graph) => graph,
};

/** The graph after one server text. Responses other than a graph, and errors, change nothing. */
export function reduce(state: GraphState, message: ServerMessage): GraphState {
  if ("result" in message) return isGraph(message.result) ? message.result : state;
  if ("error" in message) return state;
  if (state === null) return state;
  const handler = handlers[message.method] as ((graph: Graph, params: unknown) => Graph) | undefined;
  return handler === undefined ? state : handler(state, message.params);
}

/**
 * The current graph and who is watching it, outside React (App reads it with
 * useSyncExternalStore). Each server text goes through reduce(); listeners hear a new graph
 * only when one was made.
 */
export class GraphStore {
  private state: GraphState = null;
  private readonly listeners = new Set<() => void>();

  get(): GraphState {
    return this.state;
  }

  apply(message: ServerMessage): void {
    const next = reduce(this.state, message);
    if (next === this.state) return;
    this.state = next;
    for (const listener of [...this.listeners]) listener();
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }
}
