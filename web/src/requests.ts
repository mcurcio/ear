// The requests this editor sent and ear has not answered yet, each with what asked for it, pure
// (docs/editor.md#requests-in-flight). ear still draws every result (docs/editor.md#its-rules): what is kept here
// draws only the request (a ghost wire, a spinner, Saving… and Adding…), never what it asks for. A reply clears a
// request and a closed connection clears them all at once; a refused route request is kept for a second more, so
// its ghost can turn the error colour before it goes. Times are on the monotonic clock, as marks.ts's are.
import { edgeId } from "./connect";
import type { Methods, RouteEnds } from "./protocol";
import { wireEnds } from "./wires";

export type LifecycleMethod = "node.start" | "node.stop" | "node.restart" | "node.disable" | "node.enable";

/** The lifecycle bar's words for each of its methods. */
export const lifecycleLabels: Record<LifecycleMethod, string> = {
  "node.start": "Start",
  "node.stop": "Stop",
  "node.restart": "Restart",
  "node.disable": "Disable",
  "node.enable": "Enable",
};

/** What asked for a request: a route's two ends, a node's name (and so its button), or the drawer or the form. */
export type Asked =
  | { method: "route.add" | "route.remove"; ends: RouteEnds }
  | { method: "node.add" | "node.remove" | "node.update" | "node.setText" | LifecycleMethod; node: string }
  | { method: "node.addText" | "graphFile.set" };

export type AskedMethod = Asked["method"];

/** What `method` with `params` asks for; a route keeps only its two ends. */
export function askedOf<M extends AskedMethod>(method: M, params: Methods[M]["params"]): Asked {
  switch (method) {
    case "route.add":
    case "route.remove": {
      const { from, to } = params as RouteEnds;
      return { method, ends: { from, to } };
    }
    case "node.addText":
    case "graphFile.set":
      return { method } as Asked;
    default:
      return { method, node: (params as { name: string }).name } as Asked;
  }
}

/** A toast's "what was asked": `route noise:noise.mono → rec:FX`, `Restart desk`, `save ear.conf`. */
export function describeAsked(asked: Asked): string {
  switch (asked.method) {
    case "route.add":
      return `route ${wireEnds(asked.ends)}`;
    case "route.remove":
      return `remove route ${wireEnds(asked.ends)}`;
    case "node.add":
      return `add ${asked.node}`;
    case "node.addText":
      return "add node";
    case "node.remove":
      return `remove ${asked.node}`;
    case "node.update":
      return `${asked.node}'s settings`;
    case "node.setText":
      return `save ${asked.node}'s text`;
    case "graphFile.set":
      return "save ear.conf";
    default:
      return `${lifecycleLabels[asked.method]} ${asked.node}`;
  }
}

/** How long a refused route request's ghost stays, in the error colour. */
export const ghostRefusedMs = 1000;

export interface InFlight {
  token: number;
  asked: Asked;
  since: number;
}

export interface RefusedRoute {
  token: number;
  ends: RouteEnds;
  /** On the monotonic clock: when its ghost goes. */
  until: number;
}

export interface RequestsView {
  /** In the order they were sent. */
  inFlight: readonly InFlight[];
  refused: readonly RefusedRoute[];
}

export const noRequests: RequestsView = Object.freeze({ inFlight: Object.freeze([]), refused: Object.freeze([]) });

/** How a request ended: ear answered it, ear refused it, or it had no answer (the connection closed). */
export type Outcome = "answered" | "refused" | "lost";

export class Requests {
  private view: RequestsView = noRequests;
  private next = 1;
  private readonly listeners = new Set<() => void>();

  constructor(private readonly now: () => number = () => performance.now()) {}

  /** A request sent; its token ends it. */
  begin(asked: Asked): number {
    const token = this.next++;
    this.set({ ...this.view, inFlight: [...this.view.inFlight, { token, asked, since: this.now() }] });
    return token;
  }

  /** A request answered, refused or lost; a token already cleared (by a closed connection) changes nothing. */
  end(token: number, outcome: Outcome): void {
    const ended = this.view.inFlight.find((request) => request.token === token);
    if (ended === undefined) return;
    const inFlight = this.view.inFlight.filter((request) => request !== ended);
    const refused =
      outcome === "refused" && ended.asked.method === "route.add"
        ? [...this.view.refused, { token, ends: ended.asked.ends, until: this.now() + ghostRefusedMs }]
        : this.view.refused;
    this.set({ inFlight, refused });
  }

  /** The connection closed: every request is forgotten at once, and every refused ghost with them. */
  clear(): void {
    if (this.view.inFlight.length === 0 && this.view.refused.length === 0) return;
    this.set(noRequests);
  }

  /** Drops the refused ghosts whose second is over. */
  expire(): void {
    const now = this.now();
    const refused = this.view.refused.filter((ghost) => ghost.until > now);
    if (refused.length !== this.view.refused.length) this.set({ ...this.view, refused });
  }

  /** When the soonest refused ghost goes; undefined when none shows. */
  nextChange(): number | undefined {
    let soonest: number | undefined;
    for (const ghost of this.view.refused) if (soonest === undefined || ghost.until < soonest) soonest = ghost.until;
    return soonest;
  }

  /** The same object until a change, as useSyncExternalStore needs. */
  get(): RequestsView {
    return this.view;
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private set(view: RequestsView): void {
    this.view = view;
    for (const listener of [...this.listeners]) listener();
  }
}

/** A route request's ghost wire (docs/editor.md#requests-in-flight). */
export interface GhostWire {
  id: string;
  ends: RouteEnds;
  refused: boolean;
}

/**
 * One ghost per route.add in flight whose route the graph does not hold yet (once route.added draws it, the ghost is
 * hidden), then one per refused route request in its second; in flight first, in the order sent.
 */
export function ghostWires(view: RequestsView, routes: readonly RouteEnds[]): GhostWire[] {
  const held = new Set(routes.map(edgeId));
  const ghosts: GhostWire[] = [];
  for (const request of view.inFlight) {
    if (request.asked.method !== "route.add" || held.has(edgeId(request.asked.ends))) continue;
    ghosts.push({ id: `ghost:${request.token}`, ends: request.asked.ends, refused: false });
  }
  for (const ghost of view.refused) ghosts.push({ id: `ghost:${ghost.token}`, ends: ghost.ends, refused: true });
  return ghosts;
}

const lifecycleMethods = new Set<string>(Object.keys(lifecycleLabels));

/** The lifecycle methods in flight for `node`, so only their own buttons spin. */
export function lifecycleAsked(view: RequestsView, node: string): ReadonlySet<LifecycleMethod> {
  const asked = new Set<LifecycleMethod>();
  for (const { asked: a } of view.inFlight) {
    if (lifecycleMethods.has(a.method) && "node" in a && a.node === node) asked.add(a.method as LifecycleMethod);
  }
  return asked;
}

/** Whether a `method` request is in flight, for `node` when one is given and the request names one. */
export function isAsking(view: RequestsView, method: AskedMethod, node?: string): boolean {
  return view.inFlight.some(
    ({ asked }) => asked.method === method && (node === undefined || !("node" in asked) || asked.node === node),
  );
}
