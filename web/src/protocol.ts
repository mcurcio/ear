// The control port's wire format: JSON-RPC 2.0 over WebSocket at /rpc, named params only
// (docs/design/batch2.md section 6). The one home of the wire types on the UI side; the server
// side is ear_control (src/control). Both are tested against the fixtures in test/protocol.
// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.

export type JsonValue = null | boolean | number | string | JsonValue[] | { [key: string]: JsonValue };
export type JsonObject = { [key: string]: JsonValue };

// ----- The graph -----

export type NodeState = "starting" | "running" | "failed" | "stopping";
export type RouteStatusName = "pending" | "linked" | "refused";
export type FailureCode = "engine-refused" | "start-failed";

export interface FailureReason {
  code: FailureCode;
  detail: string;
}

export interface PortRef {
  node: string;
  port: string;
}

/** One output port to one input port. */
export interface RouteEnds {
  from: PortRef;
  to: PortRef;
}

/** A node spec as the client sends it (node.add's params). Settings default to {}. */
export interface NodeSpec {
  name: string;
  type: string;
  settings?: JsonObject;
}

export interface GraphNode {
  name: string;
  type: string;
  settings: JsonObject;
  inputs: string[];
  outputs: string[];
  state: NodeState;
  /** Present only while failed. */
  reason?: FailureReason;
}

export interface GraphRoute extends RouteEnds {
  status: RouteStatusName;
  /** Present only when refused. */
  detail?: string;
}

/** graph.get's and graph.subscribe's result. Nodes in name order, routes in route order. */
export interface Graph {
  graphFrequency: number;
  blockSize: number;
  realtime: boolean;
  scheduling: string;
  types: string[];
  nodes: GraphNode[];
  routes: GraphRoute[];
}

// ----- Methods (client to server) -----

/** Each method's params and result. `undefined` params: the request carries none. */
export interface Methods {
  "graph.get": { params: undefined; result: Graph };
  "graph.subscribe": { params: undefined; result: Graph };
  "levels.subscribe": { params: undefined; result: { windowsPerSecond: number } };
  "node.add": { params: NodeSpec; result: Record<string, never> };
  "node.update": { params: { name: string; settings: JsonObject }; result: Record<string, never> };
  "node.remove": { params: { name: string }; result: Record<string, never> };
  "route.add": { params: RouteEnds; result: Record<string, never> };
  "route.remove": { params: RouteEnds; result: Record<string, never> };
}
export type MethodName = keyof Methods;

// ----- Notifications (server to client) -----

/** [peak, rms], linear (1.0 = full scale), 4 decimals. */
export type Level = [peak: number, rms: number];

export interface Notifications {
  /** state starts as "starting"; public ports only. */
  "node.added": { name: string; type: string; settings: JsonObject; inputs: string[]; outputs: string[] };
  "node.updated": { name: string; settings: JsonObject };
  /** After route.removed for each of its routes. */
  "node.removed": { name: string };
  "node.state": { name: string; state: NodeState; reason?: FailureReason };
  /** Everything the node made is gone. */
  "node.stopped": { name: string; droppedFrames: number };
  "route.added": RouteEnds;
  "route.removed": RouteEnds;
  "route.status": RouteEnds & { status: RouteStatusName; detail?: string };
  /** A complete snapshot: every port block's latest window; a node missing has no level. */
  levels: { nodes: { [node: string]: { [port: string]: Level } } };
}
export type NotificationName = keyof Notifications;

// ----- Errors -----

/** data.code of a refused command (-32000): ear_core's errorCode strings. */
export type CommandErrorCode =
  | "invalid-name"
  | "unknown-node-type"
  | "invalid-settings"
  | "duplicate-node"
  | "unknown-node"
  | "unknown-port"
  | "wrong-direction"
  | "ports-changed"
  | "unknown-route"
  | "stopping";

export const rpcErrorCodes = {
  parseError: -32700,
  invalidRequest: -32600,
  methodNotFound: -32601,
  invalidParams: -32602,
  commandRefused: -32000,
} as const;
export type RpcErrorCode = (typeof rpcErrorCodes)[keyof typeof rpcErrorCodes];

export interface RpcErrorObject {
  code: RpcErrorCode;
  message: string;
  /** Present on commandRefused only. */
  data?: { code: CommandErrorCode };
}

// ----- The envelope -----

export type RpcId = number | string | null;

export interface RpcRequest<M extends MethodName = MethodName> {
  jsonrpc: "2.0";
  id: number | string;
  method: M;
  params?: Methods[M]["params"];
}

export interface RpcResult {
  jsonrpc: "2.0";
  id: RpcId;
  result: JsonValue;
}

export interface RpcErrorResponse {
  jsonrpc: "2.0";
  id: RpcId;
  error: RpcErrorObject;
}

export interface RpcNotification<N extends NotificationName = NotificationName> {
  jsonrpc: "2.0";
  method: N;
  params: Notifications[N];
}

/** One text the server sends. */
export type ServerMessage = RpcResult | RpcErrorResponse | RpcNotification;

// ----- Every name, checked against the types at compile time -----

const methodSet: { [M in MethodName]: true } = {
  "graph.get": true,
  "graph.subscribe": true,
  "levels.subscribe": true,
  "node.add": true,
  "node.update": true,
  "node.remove": true,
  "route.add": true,
  "route.remove": true,
};
export const methodNames = Object.keys(methodSet) as MethodName[];

const notificationSet: { [N in NotificationName]: true } = {
  "node.added": true,
  "node.updated": true,
  "node.removed": true,
  "node.state": true,
  "node.stopped": true,
  "route.added": true,
  "route.removed": true,
  "route.status": true,
  levels: true,
};
export const notificationNames = Object.keys(notificationSet) as NotificationName[];

const commandErrorCodeSet: { [C in CommandErrorCode]: true } = {
  "invalid-name": true,
  "unknown-node-type": true,
  "invalid-settings": true,
  "duplicate-node": true,
  "unknown-node": true,
  "unknown-port": true,
  "wrong-direction": true,
  "ports-changed": true,
  "unknown-route": true,
  stopping: true,
};
export const commandErrorCodes = Object.keys(commandErrorCodeSet) as CommandErrorCode[];

// ----- The conformance fixtures (test/protocol/*.json) -----

/**
 * One fixture: the client texts sent (verbatim, so an unparseable one can be shown), the server
 * texts received in order (as JSON), and the graph a client holds after applying them.
 */
export interface ProtocolFixture {
  description: string;
  requests: string[];
  messages: ServerMessage[];
  graph: Graph;
}
