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
export type NodeState =
  | "starting"
  | "running"
  | "stopping"
  | "stopped"
  | "disabled"
  | "failed"
  | "reconfiguring"
  | (string & {});
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
  | "reconfiguring"
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
  | "fell-back"
  | "reshape"
  | (string & {});
/** Why a change in place fell back to a restart (cause.kind fell-back). */
export type FellBackReason =
  | "restart-asked"
  | "deadline"
  | "resource-refused"
  | "resource-lost"
  | "failed"
  | "restart-requested"
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
  | "reconfigure"
  | "reshape"
  | (string & {});

export interface FailureReason {
  code: FailureCode;
  detail: string;
}

export interface NodeCause {
  kind: CauseKind;
  /** settings, reshape and fell-back: the changed keys. */
  keys?: string[];
  /** reshape: the public ports the change adds and removes. */
  added?: string[];
  removed?: string[];
  /** fell-back: why, and in words. */
  reason?: FellBackReason;
  detail?: string;
  /** restarting: the restart's own kind (and its members). */
  restart?: {
    kind: CauseKind;
    keys?: string[];
    added?: string[];
    removed?: string[];
    reason?: FellBackReason;
    detail?: string;
  };
}

/** What a node still waits for, once its phase has lasted 2 s. */
export interface WaitingFor {
  kind:
    | "older-node"
    | "block"
    | "companion"
    | "consumer"
    | "loop"
    | "store"
    | "recording"
    | "claim"
    | "program"
    | (string & {});
  /** older-node, companion, loop, store, recording (its path), claim (its key: "udp:5004") and program (its name). */
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
  /** reconfiguring only: the keys the change in flight applies. */
  keys?: string[];
  /** reconfiguring only: the keys changed since its commit point, which the node takes once it ends. */
  next?: string[];
}

/**
 * node.set's answer: what the plan applied, and when the node's ports change, whether a reshape is in place, the ports
 * added and removed, and the routes it removed (routesDropped) or, for a dry run, would remove (routes).
 */
export interface SetResult {
  applied: Applied;
  inPlace?: boolean;
  ports?: PortsMoved;
  routesDropped?: RouteEnds[];
  routes?: RouteEnds[];
}

/** A change of a node's ports (feature reshape): whole port names. */
export interface PortsMoved {
  added: string[];
  removed: string[];
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
export type CompanionKind =
  | "card-playback"
  | "card-capture"
  | "rtp-receiver"
  | "pipe-receiver"
  | "sink"
  | "source"
  | (string & {});

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
  /**
   * A companion a change in place replaces make-before-break, until the commit point destroys it: listed just before
   * its replacement, which shares its name for a moment. Absent otherwise.
   */
  retiring?: true;
  /** A sink: frames its ring had no room for, this configuration. */
  dropped?: number;
  /** A source: frames played as zeros while its writer was not idle, this configuration. */
  shortfall?: number;
}

/** A status field's value (feature status): null while unknown, a flag, a number, or a text (a choice's value is its text). */
export type StatusValue = null | boolean | number | string;
/** How an event reads to an operator; not a graph file diagnostic's Severity. */
export type EventSeverity = "info" | "warn" | "error" | (string & {});
/**
 * One of a node's diagnostics (feature status, docs/control-port.md#node-diagnostics), kept per code and subject: a
 * condition is active until it ends; an observation is never active. Its code's fields ride beside the rest. A code
 * this editor does not know is drawn by its msg.
 */
export interface NodeDiagnostic {
  code: string;
  /** What it is about: a field, a loop, a claim, a hook or a companion; "" when its code has none. */
  subject: string;
  active: boolean;
  /** A condition's times it became active, an observation's times seen. */
  count: number;
  t: string;
  msg: string;
  detail?: string;
  [field: string]: JsonValue | undefined;
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
  /** Feature counters: its counters (zero in node.added; a change of ports keeps them). */
  counters?: NodeCounters;
  /** Feature companions: its companion list, in request order ([] in node.added, and for a node with none). */
  companions?: Companion[];
  /** Feature status: every status field of its type at its value now (initial values in node.added). */
  status?: { [field: string]: StatusValue };
  /** Feature status: every diagnostic the node was told, one per code and subject ([] in node.added). */
  diagnostics?: NodeDiagnostic[];
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
  /** Feature types: the hash of the node types' schemas (types.get's), opaque; fetch them once per hash. */
  typeSchemaHash?: string;
}

// ----- The node types' schemas (feature types, docs/control-port.md#type-schemas) -----

/** A setting's value kind, as the control port takes it: a rate is a whole number of Hz there; a decimal any JSON number. */
export type SettingValueKind = "text" | "wholeNumber" | "decimal" | "flag" | (string & {});
/** How a decimal's control maps its travel to the value: evenly, on the logarithm, or as a level in dB. */
export type DecimalTaper = "linear" | "log" | "db" | (string & {});
/** What a change to the row asks of the node: from its row's marks. */
export type SettingChange = "inPlace" | "restart" | (string & {});
/** Where an optional row's value comes from when absent. */
export type DefaultFrom = "graphFrequency" | "nodeName" | (string & {});

/** One setting row of a type, as types.get says it; members that do not apply are absent. */
export interface SettingSchema {
  key: string;
  kind: SettingValueKind;
  required: boolean;
  change: SettingChange;
  /** restart: why a change to it restarts the node, one line (docs/control-port.md#type-schemas). */
  restartReason?: string;
  /** A change to it may change the node's ports. */
  shapes?: true;
  /** What an absent key reads, of the row's kind: a whole number, a decimal or a flag. */
  default?: JsonValue;
  defaultFrom?: DefaultFrom;
  /** wholeNumber and decimal: its range, and its unit when it has one. */
  min?: number;
  max?: number;
  unit?: string;
  /** decimal: the places a control shows, its taper, and whether `min` is silence (shown as -inf). */
  decimals?: number;
  taper?: DecimalTaper;
  minIsSilence?: true;
  /** text: what it must be, in the row's own words, and the hints for its control (ear stays the judge). */
  mustBe?: string;
  values?: string[];
  pattern?: string;
  maxLength?: number;
  /** Refused when the key named is given too. */
  onlyWithout?: string;
  help?: string;
}

/** One status field of a type (feature status). */
export interface StatusFieldSchema {
  name: string;
  kind: "flag" | "wholeNumber" | "decimal" | "choice" | "text" | (string & {});
  /** null: it starts unknown. */
  initial: StatusValue;
  /** panel: only in the node's panel; box: on its box too; callout: a banner while not at its initial value. */
  display: "panel" | "box" | "callout" | (string & {});
  flapsAbove: { transitions: number; windowMs: number };
  min?: number;
  max?: number;
  decimals?: number;
  unit?: string;
  values?: string[];
  maxBytes?: number;
  help?: string;
}

/** One event of a type (feature status): its sentence names each argument once as {name}. */
export interface EventSchema {
  code: string;
  severity: EventSeverity;
  sentence: string;
  args: Array<{ name: string; kind: string; min?: number; max?: number; decimals?: number; values?: string[]; maxBytes?: number }>;
}

/** An action's argument: a setting row without what only a setting has (change, restartReason, shapes). */
export type ActionArgSchema = Omit<SettingSchema, "change" | "restartReason" | "shapes">;

/**
 * One type action (feature actions, docs/control-port.md#actions): a trigger (target code, no args) or a call (target
 * loop, its args setting rows).
 */
export interface ActionSchema {
  name: string;
  by: "type" | (string & {});
  target: "code" | "loop" | (string & {});
  help: string;
  args: ActionArgSchema[];
}

/** One node type, as types.get says it. */
export interface TypeSchema {
  name: string;
  settings: SettingSchema[];
  /** The public banks' names, in declaration order. */
  inputs: string[];
  outputs: string[];
  /** How long a change in place may take before it falls back to a restart. */
  reconfigureMs: number;
  /** Feature status: its status fields and events, in declaration order. */
  status?: StatusFieldSchema[];
  events?: EventSchema[];
  /** Feature actions: its actions, in declaration order. */
  actions?: ActionSchema[];
}

/** types.get's result: the schemas, or notModified when ifNoneMatch named the hash. */
export type TypesResult =
  | { protocol: number; hash: string; typeSchemas: TypeSchema[]; notModified?: undefined }
  | { protocol: number; hash: string; notModified: true; typeSchemas?: undefined };

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
  /** Feature types: every node type's schema, or notModified when ifNoneMatch is the hash. */
  "types.get": { params: { ifNoneMatch?: string }; result: TypesResult };
  /** disabled (feature lifecycle): written as `disabled true;` in the node's braces. */
  "node.add": { params: NodeSpec & { disabled?: boolean }; result: Record<string, never> };
  /** dropRoutes (feature reshape): the routes its new ports strand are removed in the same edit. */
  "node.update": {
    params: { name: string; settings: JsonObject; dropRoutes?: boolean };
    result: Record<string, never>;
  };
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
  /**
   * Feature node.set: a JSON merge patch of the node's settings (null removes a key), answered at the commit with what
   * the plan applied. dryRun (feature dryRun): the same answer, nothing changed; dropRoutes (feature reshape): the routes
   * its new ports strand are removed in the same edit.
   */
  "node.set": {
    params: { name: string; settings: JsonObject; dryRun?: boolean; dropRoutes?: boolean };
    result: SetResult;
  };
  /** The lifecycle (feature lifecycle). Start, stop and restart edit nothing; disable and enable edit ear.conf. */
  "node.start": { params: { name: string }; result: { applied: Applied } };
  "node.stop": { params: { name: string }; result: { applied: Applied } };
  "node.restart": { params: { name: string }; result: { applied: Applied } };
  "node.disable": { params: { name: string }; result: { applied: Applied } };
  "node.enable": { params: { name: string }; result: { applied: Applied } };
  /** Feature actions: one of the node's type actions, args an object; {} once done (docs/control-port.md#actions). */
  "node.invoke": { params: { name: string; action: string; args?: JsonObject }; result: Record<string, never> };
}
export type MethodName = keyof Methods;

// ----- Notifications (server to client) -----

/** [peak, rms], linear (1.0 = full scale), 4 decimals. */
export type Level = [peak: number, rms: number];

export interface Notifications {
  /** Public ports only; its health from its spec (added, or disabled). */
  "node.added": GraphNode;
  /**
   * applied: what the change asks of the node; disabled only when true. Feature reshape: when its ports changed, the
   * ports added and removed and its new inputs and outputs, and on a reshape whether it is taken in place.
   */
  "node.updated": {
    name: string;
    settings: JsonObject;
    applied?: Applied;
    disabled?: true;
    inPlace?: boolean;
    ports?: PortsMoved;
    inputs?: string[];
    outputs?: string[];
  };
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
  /**
   * Feature status: the fields changed since the node's last (merged into its status), at most 30 a second per node;
   * with reset, every field at its initial value (replacing its status) after its configuration closed.
   */
  "node.statusChanged": { name: string; fields: { [field: string]: StatusValue }; reset?: true };
  /** Feature status: an event the node raised; nothing in the graph changes. */
  "node.eventRaised": {
    name: string;
    code: string;
    severity: EventSeverity;
    t: string;
    args: { [arg: string]: StatusValue };
    msg: string;
  };
  /** Feature status: one diagnostic, replacing the node's entry of the same code and subject, or joining its list. */
  "node.diagnostic": { name: string } & NodeDiagnostic;
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
  /** node.update without dropRoutes: its new ports drop ports routes name (data.ports, data.routes). */
  | "ports-in-use"
  /** node.invoke (feature actions): no such action of the node's type (data.action). */
  | "unknown-action"
  /** node.invoke: the arguments fail the action's table (data.key, data.detail). */
  | "invalid-args"
  /** node.invoke: the node cannot take it now (data.phase). */
  | "not-running"
  /** node.invoke: the configuration was torn down before the call ran. */
  | "restarted"
  /** node.invoke: the handler refused; the message is its text (data.detail). */
  | "action-failed"
  /** node.invoke: no answer within 10 s. */
  | "action-timeout"
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
    /** ports-in-use: the dropped ports routes name, and every such route. */
    ports?: string[];
    routes?: RouteEnds[];
    /** invalidParams. */
    pointer?: string;
    /**
     * invalid-settings and invalid-args: the key the refusal names (absent for a rule naming none), and the refusal in
     * its own words; action-failed: the handler's text.
     */
    key?: string;
    detail?: string;
    /** unknown-action: the action asked for. */
    action?: string;
    /** not-running: the node's phase. */
    phase?: string;
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
  "types.get": true,
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
  "node.set": true,
  "node.setText": true,
  "node.addText": true,
  "node.invoke": true,
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
  "node.statusChanged": true,
  "node.eventRaised": true,
  "node.diagnostic": true,
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

const controlErrorCodeSet: {
  [C in
    | "node-disabled"
    | "not-restartable"
    | "ports-in-use"
    | "unknown-action"
    | "invalid-args"
    | "not-running"
    | "restarted"
    | "action-failed"
    | "action-timeout"]: true;
} = {
  "node-disabled": true,
  "not-restartable": true,
  "ports-in-use": true,
  "unknown-action": true,
  "invalid-args": true,
  "not-running": true,
  restarted: true,
  "action-failed": true,
  "action-timeout": true,
};
export const controlErrorCodes = Object.keys(controlErrorCodeSet) as ControlErrorCode[];

/** The features this editor uses when ear says it has them (docs/control-port.md#protocol-and-features). */
export const lifecycleFeature = "lifecycle";
export const countersFeature = "counters";
export const companionsFeature = "companions";
export const nodeTextFeature = "node-text";
export const reshapeFeature = "reshape";
export const nodeSetFeature = "node.set";
export const dryRunFeature = "dryRun";
export const typesFeature = "types";
export const actionsFeature = "actions";

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
