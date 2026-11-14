// The control port's wire format on the editor's side: JSON-RPC 2.0 over WebSocket at /rpc, named params only
// (docs/control-port.md, the wire's one description). The one home of the wire types on the UI side; the server side is
// ear_control (src/control). Both are tested against the fixtures in test/protocol, and a change to the wire changes
// that page, the fixtures and both sides together (docs/control-port.md#changing-the-wire).
//
// Every enum ear sends is open from protocol 1 (docs/control-port.md#protocol-and-features): a known union plus
// (string & {}), so a value this editor does not know types and is drawn neutrally, never a failure.

export type JsonValue = null | boolean | number | string | JsonValue[] | { [key: string]: JsonValue };
export type JsonObject = { [key: string]: JsonValue };

// ----- The graph -----

/** The phase's colour (docs/control-port.md#node-health). */
export type NodeState = "starting" | "running" | "stopping" | "stopped" | "disabled" | "failed" | (string & {});
/** The exact state; a client explains by it and never colours by it. */
export type NodePhase =
  | "added"
  | "configuring"
  | "configured"
  | "starting"
  | "running"
  | "stopping"
  | "stopped"
  | "disabled"
  | "failed"
  | (string & {});
/** What the graph wants of the node: disabled from ear.conf, stopped while its stop is held, else running. */
export type Wanted = "running" | "stopped" | "disabled" | (string & {});
/** Why a node is stopping, or configuring after a restart, a retry or an enable. */
export type CauseKind =
  | "stop"
  | "removing"
  | "disabling"
  | "restarting"
  | "settings"
  | "action"
  | "retry"
  | "enabling"
  | (string & {});
export type RouteStatusName = "pending" | "linked" | "refused" | (string & {});
export type FailureCode = "engine-refused" | "start-failed" | "lost" | (string & {});
/** What a lifecycle request applied, or (node.updated) what a change asked of the node. */
export type Applied =
  | "start"
  | "stop"
  | "restart"
  | "retry"
  | "disable"
  | "enable"
  | "stored"
  | "unchanged"
  | (string & {});

export interface FailureReason {
  code: FailureCode;
  detail: string;
}

export interface NodeCause {
  kind: CauseKind;
  /** settings: the changed keys. */
  keys?: string[];
  /** restarting: the restart's own kind (and its keys). */
  restart?: { kind: CauseKind; keys?: string[] };
}

/** What a node still waits for, once its phase has lasted 2 s. */
export interface WaitingFor {
  kind: "older-node" | "block" | "companion" | "consumer" | "loop" | "store" | "recording" | "claim" | (string & {});
  /** older-node, companion, loop, store, recording (its path) and claim (its key: "udp:5004"). */
  name?: string;
}

/** A node's health (docs/control-port.md#node-health): node.state replaces every member, so an absent one is absent now. */
export interface NodeHealth {
  state: NodeState;
  phase: NodePhase;
  /** When this phase began, UTC to the microsecond. */
  since?: string;
  wanted: Wanted;
  /** The engine's block is lost: neither Start nor restart can bring the node back. */
  blockLost: boolean;
  cause?: NodeCause;
  waitingFor?: WaitingFor[];
  /** Present only while failed. */
  reason?: FailureReason;
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

/**
 * The diagnostics counters (feature counters, docs/control-port.md#counters): running totals that never step back, a
 * JSON number each. The names are an open set: one this editor does not know is kept, and shown by its name and number.
 */
export interface Counters {
  [counter: string]: number;
}

/** The graph's: what the host did to every output at once. */
export interface GraphCounters extends Counters {
  /** Cycles the internal clock started since ear started. */
  cycles: number;
  /** Of them, those the driver cut short: a dropout on every output. */
  cutCycles: number;
  /** Times the graph fell behind and lost cycles, one each however many cycles it cut. */
  stalls: number;
  /** The time those cut cycles lost. */
  lostMs: number;
}

/** A node's, from its add: what happened to that node beyond the graph's own losses. */
export interface NodeCounters extends Counters {
  /** Cycles its block missed that the driver did not cut (it was not scheduled). */
  missedCycles: number;
  /** Cycles in which its own work took longer than the cycle. */
  overruns: number;
  repositions: number;
  driverChanges: number;
}

/** A companion's kind (feature companions): an open set, so one this editor does not know is drawn by its own text. */
export type CompanionKind = "card-playback" | "card-capture" | "rtp-receiver" | "sink" | "source" | (string & {});

/**
 * One of a node's hidden helpers (feature companions, docs/control-port.md#companions): never a node, never routed,
 * never metered. Members ear adds later are kept as sent.
 */
export interface Companion {
  /** `<node>.playback`, `<node>.capture`, `<node>.rtp`; a sink's or source's `<node>.<bank>`. */
  name: string;
  kind: CompanionKind;
  /** The inside bank it joins: inside banks appear nowhere else on the wire. */
  bank: string;
  ready: boolean;
  /** A sink: frames its ring had no room for, this configuration. */
  dropped?: number;
  /** A source: frames played as zeros while its writer was not idle, this configuration. */
  shortfall?: number;
}

/** A node as snapshots and node.added carry it: its spec, its public ports, its health, its counters and companions. */
export interface GraphNode extends NodeHealth {
  name: string;
  type: string;
  settings: JsonObject;
  inputs: string[];
  outputs: string[];
  /** ear.conf disables it (`disabled true;` in its braces). */
  disabled?: true;
  /** Feature counters: its counters (a port-caused replace's node.added carries those it carries on). */
  counters?: NodeCounters;
  /** Feature companions: its companion list, in request order ([] in node.added, and for a node with none). */
  companions?: Companion[];
}

export interface GraphRoute extends RouteEnds {
  status: RouteStatusName;
  /** Present only when refused. */
  detail?: string;
}

/** graph.get's and graph.subscribe's result. Nodes in name order, routes in route order. */
export interface Graph {
  /** The wire's protocol (absent: 0, the wire before the lifecycle). */
  protocol?: number;
  /** What this ear speaks beyond protocol 0's: a client gates each method and request member on its feature. */
  features?: string[];
  graphFrequency: number;
  blockSize: number;
  realtime: boolean;
  scheduling: string;
  types: string[];
  nodes: GraphNode[];
  routes: GraphRoute[];
  /** Feature counters: the graph's counters, read with the rest. */
  counters?: GraphCounters;
}

// ----- The graph file (docs/control-port.md#the-graph-file-over-the-port) -----

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

/** A place in ear.conf's text: line from 1; column from 1 in UTF-16 code units. */
export interface TextPlace {
  line: number;
  column: number;
}

/**
 * Where one node's statement is written in the GraphFile's text (feature node-text): from start to one past its last
 * character, the comments it owns included. shared: the statement sits in a snippet (one text, maybe several nodes),
 * so node.setText refuses it (shared-definition) and the editor only shows it.
 */
export interface NodeText {
  name: string;
  start: TextPlace;
  end: TextPlace;
  shared?: true;
}

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
  /** Feature node-text: where each node's statement is in `text`; absent when the file has errors. */
  nodeTexts?: NodeText[];
}

/** edit: an editor or pane edit; disk: ear.conf changed on disk; write: writeFailed set or cleared. */
export type FileChangeCause = "edit" | "disk" | "write";

// ----- Methods (client to server) -----

/** Each method's params and result. `undefined` params: the request carries none. */
export interface Methods {
  "graph.get": { params: undefined; result: Graph };
  "graph.subscribe": { params: undefined; result: Graph };
  "levels.subscribe": { params: undefined; result: { windowsPerSecond: number } };
  /** disabled (feature lifecycle): written as `disabled true;` in the node's braces. */
  "node.add": { params: NodeSpec & { disabled?: boolean }; result: Record<string, never> };
  "node.update": { params: { name: string; settings: JsonObject }; result: Record<string, never> };
  "node.remove": { params: { name: string }; result: Record<string, never> };
  "route.add": { params: RouteEnds; result: Record<string, never> };
  "route.remove": { params: RouteEnds; result: Record<string, never> };
  "graphFile.get": { params: undefined; result: GraphFile };
  /** The file, then every graphFile.changed. */
  "graphFile.subscribe": { params: undefined; result: GraphFile };
  /** ear.conf's whole new text and the version it was based on; {} once the graph runs it. */
  "graphFile.set": { params: { text: string; version: string }; result: Record<string, never> };
  /**
   * Feature node-text: the node's whole statement in knobly, replacing its own in ear.conf (every other byte kept),
   * against the version its text was cut from; {} once the graph runs it. invalid-text's diagnostics are placed in
   * `text`.
   */
  "node.setText": { params: { name: string; text: string; version: string }; result: Record<string, never> };
  /**
   * Feature node-text: a whole `node` statement in knobly, appended as node.add appends, against the version it was
   * written for; {} once the graph runs it. invalid-text's diagnostics are placed in `text`.
   */
  "node.addText": { params: { text: string; version: string }; result: Record<string, never> };
  /** The lifecycle (feature lifecycle). Start, stop and restart edit nothing; disable and enable edit ear.conf. */
  "node.start": { params: { name: string }; result: { applied: Applied } };
  "node.stop": { params: { name: string }; result: { applied: Applied } };
  "node.restart": { params: { name: string }; result: { applied: Applied } };
  "node.disable": { params: { name: string }; result: { applied: Applied } };
  "node.enable": { params: { name: string }; result: { applied: Applied } };
}
export type MethodName = keyof Methods;

// ----- Notifications (server to client) -----

/** [peak, rms], linear (1.0 = full scale), 4 decimals. */
export type Level = [peak: number, rms: number];

export interface Notifications {
  /** Public ports only; its health from its spec (added, or disabled). */
  "node.added": GraphNode;
  /** applied: what the change asks of the node; disabled only when true. */
  "node.updated": { name: string; settings: JsonObject; applied?: Applied; disabled?: true };
  /** After route.removed for each of its routes. */
  "node.removed": { name: string };
  /** Every health member: one absent is absent now. */
  "node.state": { name: string } & NodeHealth;
  /** Deprecated (protocol 1 sends it, then node.gone): everything the node made is gone. */
  "node.stopped": { name: string; droppedFrames: number };
  /** Everything the node made is gone (feature lifecycle). */
  "node.gone": { name: string; droppedFrames: number };
  "route.added": RouteEnds;
  "route.removed": RouteEnds;
  "route.status": RouteEnds & { status: RouteStatusName; detail?: string };
  /** A complete snapshot: every port block's latest window; a node missing has no level. */
  levels: { nodes: { [node: string]: { [port: string]: Level } } };
  /** Before the node and route events it causes. droppedEdits (disk only): accepted edits not yet written, now gone. */
  "graphFile.changed": GraphFile & { cause: FileChangeCause; droppedEdits?: number };
  /**
   * Feature counters, at most once a second and only when a loss moved: the graph's counters whole, and the whole
   * counters of each node whose counters changed. Replaces the counters it names; a node not held is ignored.
   */
  "counters.changed": { graph: GraphCounters; nodes: { [node: string]: NodeCounters } };
  /**
   * Feature companions: the node's whole companion list, whenever an entry is added or removed, its readiness changes
   * or (at most once a second) a counter moved. Replaces the list; a node not held is ignored.
   */
  "node.companionsChanged": { name: string; companions: Companion[] };
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

/** data.code of the control plane's own refusals (-32000; docs/control-port.md#error-codes). */
export type ControlErrorCode =
  /** node.start, node.stop or node.restart on a node ear.conf disables. */
  | "node-disabled"
  /** node.start or node.restart: the block is lost; node.restart: not started yet (data.reason). */
  | "not-restartable"
  | (string & {});

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
  /** commandRefused: its code and what it names; invalidParams: the JSON pointer of the param at fault. */
  data?: {
    code?: CommandErrorCode | FileErrorCode | ControlErrorCode;
    diagnostics?: Diagnostic[];
    line?: number;
    column?: number;
    /** not-restartable: "block-lost" or "not-started". */
    reason?: string;
    /** invalidParams. */
    pointer?: string;
  };
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
  "node.start": true,
  "node.stop": true,
  "node.restart": true,
  "node.disable": true,
  "node.enable": true,
  "node.setText": true,
  "node.addText": true,
};
export const methodNames = Object.keys(methodSet) as MethodName[];

const notificationSet: { [N in NotificationName]: true } = {
  "node.added": true,
  "node.updated": true,
  "node.removed": true,
  "node.state": true,
  "node.stopped": true,
  "node.gone": true,
  "route.added": true,
  "route.removed": true,
  "route.status": true,
  levels: true,
  "graphFile.changed": true,
  "counters.changed": true,
  "node.companionsChanged": true,
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

const controlErrorCodeSet: { [C in "node-disabled" | "not-restartable"]: true } = {
  "node-disabled": true,
  "not-restartable": true,
};
export const controlErrorCodes = Object.keys(controlErrorCodeSet) as ControlErrorCode[];

/** The features this editor uses when ear says it has them (docs/control-port.md#protocol-and-features). */
export const lifecycleFeature = "lifecycle";
export const countersFeature = "counters";
export const companionsFeature = "companions";
export const nodeTextFeature = "node-text";

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
