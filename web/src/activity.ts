// A node's status, events and diagnostics as the editor shows them (docs/editor.md#status-events-and-diagnostics): the
// one place that turns a node's status (docs/control-port.md#status) into the panel's list and the box's fields and
// callouts, its active diagnostics (docs/control-port.md#node-diagnostics) into the box's badges, and each event and
// diagnostic into the node's activity list and, for a warn or error event, a toast. Nothing here knows a type's or a
// code's name: what to show comes from the type's schema and the entries' own msg.
import type { NoticeInput } from "./notices";
import type {
  GraphNode,
  NodeDiagnostic,
  Notifications,
  ServerMessage,
  StatusFieldSchema,
  StatusValue,
  TypeSchema,
} from "./protocol";

/** How many entries a node's activity list keeps: the newest. */
export const activityCap = 100;
/** At most one new toast per node in this long; the notices queue coalesces the same code within it too. */
export const noticePerNodeMs = 5000;

/** A value in words: a flag yes or no, null unknown, a number or a text as it is. */
export function valueWords(value: StatusValue | undefined): string {
  if (value === null || value === undefined) return "unknown";
  if (typeof value === "boolean") return value ? "yes" : "no";
  return String(value);
}

/** One row of the panel's status list: every field the node holds, in the schema's order when it is held. */
export interface StatusRow {
  name: string;
  text: string;
  help?: string;
}

/** The node's status as the panel lists it: the schema's fields in order (then any the schema lacks), each in words. */
export function statusRows(node: GraphNode, schema?: TypeSchema): StatusRow[] {
  const status = node.status ?? {};
  const rows: StatusRow[] = [];
  const named = new Set<string>();
  for (const field of schema?.status ?? []) {
    if (!(field.name in status)) continue;
    named.add(field.name);
    rows.push({ name: field.name, text: valueWords(status[field.name]), ...(field.help ? { help: field.help } : {}) });
  }
  for (const [name, value] of Object.entries(status))
    if (!named.has(name)) rows.push({ name, text: valueWords(value) });
  return rows;
}

/** The states in which a box shows its fields and callouts: the node's configuration holds them. */
const showingStates = new Set(["running", "reconfiguring", "stopped"]);

/** What the box draws of the status: its box fields after the state line, and a banner per callout. */
export interface BoxStatus {
  /** The box fields' values, joined by " · " ("connected · Music Assistant"); "" for none. */
  line: string;
  callouts: Array<{ name: string; text: string }>;
}

function isInitial(field: StatusFieldSchema, value: StatusValue): boolean {
  return value === field.initial;
}

/**
 * The box's fields and callouts: only while the node is running, reconfiguring or stopped, and never a null value; a
 * callout only while it is not at its initial value. Without the type's schema, none (where to show a field is the
 * schema's to say).
 */
export function boxStatus(node: GraphNode, schema?: TypeSchema): BoxStatus {
  const none: BoxStatus = { line: "", callouts: [] };
  if (!showingStates.has(node.state) || node.status === undefined || schema?.status === undefined) return none;
  const values: string[] = [];
  const callouts: BoxStatus["callouts"] = [];
  for (const field of schema.status) {
    const value = node.status[field.name];
    if (value === null || value === undefined) continue;
    if (field.display === "box") values.push(valueWords(value));
    else if (field.display === "callout" && !isInitial(field, value))
      callouts.push({ name: field.name, text: valueWords(value) });
  }
  return { line: values.join(" · "), callouts };
}

/** One badge on the box: an active diagnostic, its words and its tooltip. */
export interface DiagnosticBadge {
  key: string;
  label: string;
  title: string;
}

/** A code in a few words: the framework's prefix dropped, flapping said as such ("flapping: connection"). */
export function diagnosticLabel(diagnostic: NodeDiagnostic): string {
  const word =
    diagnostic.code === "ear.status-flapping" ? "flapping" : diagnostic.code.replace(/^ear\./, "").replaceAll("-", " ");
  return diagnostic.subject !== "" ? `${word}: ${diagnostic.subject}` : word;
}

/** The tooltip: its sentence (an unknown code's msg), how often, and for flapping its threshold. */
export function diagnosticTitle(diagnostic: NodeDiagnostic): string {
  const sentence = typeof diagnostic.detail === "string" ? diagnostic.detail : diagnostic.msg;
  const times = diagnostic.count === 1 ? "once" : `${diagnostic.count} times`;
  const limit =
    typeof diagnostic.transitions === "number" && typeof diagnostic.windowMs === "number"
      ? `; ${diagnostic.transitions} changes in ${diagnostic.windowMs / 1000} s`
      : "";
  return `${sentence} (${times}${limit})`;
}

/** A badge for each active entry, in the node's order; an observation never has one. */
export function diagnosticBadges(node: GraphNode): DiagnosticBadge[] {
  return (node.diagnostics ?? [])
    .filter((diagnostic) => diagnostic.active)
    .map((diagnostic) => ({
      key: `${diagnostic.code}\n${diagnostic.subject}`,
      label: diagnosticLabel(diagnostic),
      title: diagnosticTitle(diagnostic),
    }));
}

/** One entry of a node's activity list. */
export interface ActivityEntry {
  kind: "event" | "diagnostic";
  code: string;
  /** An event's severity; a diagnostic's is "active" or "inactive". */
  severity: string;
  t: string;
  msg: string;
}

/**
 * Each node's activity since this page connected: its events and diagnostics, the newest last, at most activityCap per
 * node; and a toast for a warn or error event, at most one new one per node per noticePerNodeMs. Kept by node name; a
 * node removed keeps its list until one of the name is added again.
 */
export class Activity {
  private readonly byNode = new Map<string, ActivityEntry[]>();
  private readonly lastNotice = new Map<string, number>();
  private readonly listeners = new Set<() => void>();

  constructor(private readonly now: () => number = () => performance.now()) {}

  /** The node's entries, oldest first. */
  of(node: string): readonly ActivityEntry[] {
    return this.byNode.get(node) ?? [];
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  /** One server text: an event or a diagnostic joins its node's list; a warn or error event may be a toast. */
  apply(message: ServerMessage): NoticeInput | undefined {
    if (!("method" in message)) return undefined;
    if (message.method === "node.added") {
      this.byNode.delete((message.params as Notifications["node.added"]).name);
      return undefined;
    }
    if (message.method === "node.eventRaised") {
      const event = message.params as Notifications["node.eventRaised"];
      this.push(event.name, { kind: "event", code: event.code, severity: event.severity, t: event.t, msg: event.msg });
      if (event.severity !== "warn" && event.severity !== "error") return undefined;
      const now = this.now();
      const last = this.lastNotice.get(event.name);
      if (last !== undefined && now - last < noticePerNodeMs) return undefined;
      this.lastNotice.set(event.name, now);
      return {
        severity: event.severity === "error" ? "error" : "warning",
        code: event.code,
        what: event.name,
        verb: "says",
        detail: event.msg,
        subject: { kind: "node", node: event.name },
      };
    }
    if (message.method === "node.diagnostic") {
      const { name, ...diagnostic } = message.params as Notifications["node.diagnostic"];
      this.push(name, {
        kind: "diagnostic",
        code: diagnostic.code,
        severity: diagnostic.active ? "active" : "inactive",
        t: diagnostic.t,
        msg: diagnostic.msg,
      });
    }
    return undefined;
  }

  private push(node: string, entry: ActivityEntry): void {
    const list = [...(this.byNode.get(node) ?? []), entry];
    this.byNode.set(node, list.length > activityCap ? list.slice(list.length - activityCap) : list);
    for (const listener of [...this.listeners]) listener();
  }
}
