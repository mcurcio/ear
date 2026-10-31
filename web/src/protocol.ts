// The control port's wire format: JSON-RPC 2.0 over WebSocket at /rpc, named params only
// (docs/design/batch2.md section 6; the graph file's methods: docs/design/knobly-config.md,
// "Wire additions"). The one home of the wire types on the UI side; the server
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

// ----- The graph file (docs/design/knobly-config.md, "Wire additions") -----

export type Severity = "error" | "warning";

/** A problem in ear.conf's text. line from 1; column from 1 in UTF-16 code units. */
export interface Diagnostic {
  severity: Severity;
  /** knobly's ("E2001") or ear's ("ear/include-unsupported"). */
  code: string;
  message: string;
  line: number;
  column: number;
}

/** The desired graph refused the file's version, at the line that made the refused item if known. */
export interface Refusal {
  code: CommandErrorCode;
  message: string;
  line?: number;
  column?: number;
}

export type FormatKey = "graphFrequency" | "blockSize";

/** graphFile.get's and graphFile.subscribe's result, and graphFile.changed's params. */
export interface GraphFile {
  /** Opaque; changes whenever the text changes. */
  version: string;
  /** ear.conf; "" when missing or unreadable. */
  text: string;
  /** Any error means the file does not apply. */
  diagnostics: Diagnostic[];
  /** The version the graph runs; absent: none yet. */
  running?: string;
  /** The desired graph refused `version`. */
  refusal?: Refusal;
  /** The file's settings that differ from the engine's and apply at the next start. */
  pendingRestart: FormatKey[];
  /** The disk refused the last write. */
  writeFailed?: string;
}

/** edit: an editor or pane edit; disk: ear.conf changed on disk; write: writeFailed set or cleared. */
export type FileChangeCause = "edit" | "disk" | "write";

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
  "graphFile.get": { params: undefined; result: GraphFile };
  /** The file, then every graphFile.changed. */
  "graphFile.subscribe": { params: undefined; result: GraphFile };
  /** ear.conf's whole new text and the version it was based on; {} once the graph runs it. */
  "graphFile.set": { params: { text: string; version: string }; result: Record<string, never> };
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
  /** Before the node and route events it causes. droppedEdits (disk only): accepted edits not yet written, now gone. */
  "graphFile.changed": GraphFile & { cause: FileChangeCause; droppedEdits?: number };
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
  | "unknown-route"
  | "stopping";

/** data.code of a refused graph file edit (-32000). */
export type FileErrorCode =
  /** graphFile.set: version is stale. */
  | "file-changed"
  /** graphFile.set: the text has errors (data.diagnostics). */
  | "invalid-text"
  /** a node.* or route.* edit while the file has errors. */
  | "file-invalid"
  /** the line also makes other nodes or routes (data.line, data.column). */
  | "shared-definition";

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
  data?: { code: CommandErrorCode | FileErrorCode; diagnostics?: Diagnostic[]; line?: number; column?: number };
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

/** One message the server sends. */
export type ServerMessage = RpcResult | RpcErrorResponse | RpcNotification;

/** One text the server sends: a message, or a batch's responses in one array. */
export type ServerText = ServerMessage | ServerMessage[];

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
  "graphFile.get": true,
  "graphFile.subscribe": true,
  "graphFile.set": true,
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
  "graphFile.changed": true,
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
  "unknown-route": true,
  stopping: true,
};
export const commandErrorCodes = Object.keys(commandErrorCodeSet) as CommandErrorCode[];

const fileErrorCodeSet: { [C in FileErrorCode]: true } = {
  "file-changed": true,
  "invalid-text": true,
  "file-invalid": true,
  "shared-definition": true,
};
export const fileErrorCodes = Object.keys(fileErrorCodeSet) as FileErrorCode[];

// ----- The conformance fixtures (test/protocol/*.json) -----

/**
 * One fixture: the client texts sent (verbatim, so an unparseable one can be shown), the server
 * texts received in order (as JSON), the graph a client holds after applying them, and the graph
 * file it holds (where the fixture subscribes to it).
 */
export interface ProtocolFixture {
  description: string;
  requests: string[];
  messages: ServerText[];
  graph: Graph;
  file?: GraphFile;
}
