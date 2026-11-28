// The editor's toasts (docs/editor.md#toasts): the session's store over patchbay's toast queue (its addToast,
// holdToasts, expireToasts and visibleToasts), so the session raises a toast outside React and patchbay's Toaster
// draws it. patchbay's model sets the rules (D11): a toast lasts 8 s on the browser's monotonic clock, the same key
// within 5 s counts up instead of stacking, the newest four show, and none expires while held. ear keys a toast by its
// code and its subject, and keeps a closed connection from clearing any, so a reconnect drops nothing the person has
// not seen. Also here: a refusal in words (`route noise:noise.mono → rec:FX refused: unknown-port rec:FX`), which
// refusals a form shows in place, and what a toast's Show selects. Slice 5's node events are designed to feed this
// same queue by its rule (docs/editor.md#toasts), so NoticeInput is plain words, a severity and a subject.
import {
  addToast,
  dismissToast,
  emptyToastQueue,
  expireToasts,
  holdToasts,
  nextToastExpiry,
  visibleToasts,
  type Toast,
  type ToastQueue,
  type ToastTone,
} from "@mcurcio/patchbay/hooks";
import { edgeId } from "./connect";
import { rpcErrorCodes, type Graph, type Notifications, type RouteEnds } from "./protocol";
import { describeAsked, type Asked } from "./requests";
import { ConnectionClosedError, RpcError } from "./rpc";
import type { Selection } from "./selection";

export type NoticeSeverity = "error" | "warning" | "info";

/** What a toast is about, so Show can select it. */
export type NoticeSubject = { kind: "node"; node: string } | { kind: "route"; ends: RouteEnds };

/** A toast as added; it reads `${what} ${verb}: <code>{code}</code> ${detail}`. */
export interface NoticeInput {
  severity: NoticeSeverity;
  code: string;
  what: string;
  verb: string;
  detail: string;
  subject?: NoticeSubject;
}

/** A toast in the queue (its id, count, lastAt and expiresAt patchbay's) with the words it was last added with. */
export type Notice = Toast & NoticeInput;

export interface NoticesView {
  /** Every toast, oldest first. */
  notices: readonly Notice[];
  /** The newest four, oldest first: the ones the Toaster draws. */
  shown: readonly Notice[];
  /** How many older ones fold into "and n more". */
  more: number;
  /** While held: since when. */
  heldSince?: number;
}

const noNotices: NoticesView = Object.freeze({ notices: Object.freeze([]), shown: Object.freeze([]), more: 0 });

/** The key two notices coalesce by: the code and the subject. */
export function noticeKey(code: string, subject?: NoticeSubject): string {
  if (subject === undefined) return `${code}\n`;
  return subject.kind === "node" ? `${code}\nnode ${subject.node}` : `${code}\nroute ${edgeId(subject.ends)}`;
}

/**
 * A severity as patchbay's toast tones: a refusal is an `err` toast (read at once), the rest status; info is a tone
 * patchbay does not know, so it is drawn neutrally.
 */
export const toneOf: Record<NoticeSeverity, ToastTone> = { error: "err", warning: "warn", info: "info" };

/** A notice in plain words, the toast's text in the queue; the Toaster draws the code in monospace. */
export function noticeWords(input: NoticeInput): string {
  return `${input.what} ${input.verb}: ${input.code}${input.detail === "" ? "" : ` ${input.detail}`}`;
}

export class Notices {
  private queue: ToastQueue = emptyToastQueue;
  /** Each toast's words as last added, by its id. */
  private inputs = new Map<number, NoticeInput>();
  private view: NoticesView = noNotices;
  private readonly listeners = new Set<() => void>();

  constructor(private readonly now: () => number = () => performance.now()) {}

  add(input: NoticeInput): void {
    const toast = { tone: toneOf[input.severity], text: noticeWords(input), key: noticeKey(input.code, input.subject) };
    const next = addToast(this.queue, toast, this.now());
    this.inputs.set(next.toasts.at(-1)!.id, input);
    this.set(next);
  }

  dismiss(id: number): void {
    this.set(dismissToast(this.queue, id));
  }

  /** Held (hovered or focused), no toast expires; let go, each expiry moves on by the time held. */
  hold(held: boolean): void {
    this.set(holdToasts(this.queue, held, this.now()));
  }

  /** Drops the toasts whose time is over; nothing while held. */
  expire(): void {
    this.set(expireToasts(this.queue, this.now()));
  }

  /** When the soonest toast expires; undefined while held or with none. */
  nextExpiry(): number | undefined {
    return nextToastExpiry(this.queue);
  }

  /** The same object until a change, as useSyncExternalStore needs. */
  get(): NoticesView {
    return this.view;
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private set(queue: ToastQueue): void {
    if (queue === this.queue) return;
    this.queue = queue;
    const notice = (toast: Toast): Notice => ({ ...this.inputs.get(toast.id)!, ...toast });
    const notices = queue.toasts.map(notice);
    const ids = new Set(queue.toasts.map((toast) => toast.id));
    for (const id of this.inputs.keys()) if (!ids.has(id)) this.inputs.delete(id);
    const { shown, more } = visibleToasts(queue);
    const view: NoticesView = { notices, shown: shown.map(notice), more };
    if (queue.heldSince !== undefined) view.heldSince = queue.heldSince;
    this.view = view;
    for (const listener of [...this.listeners]) listener();
  }
}

/** A refusal's words: the verb, the code (in monospace) and the rest of ear's message. */
export interface RefusalParts {
  verb: string;
  code: string;
  detail: string;
  severity: NoticeSeverity;
}

const rpcCodeWords: Record<number, string> = {
  [rpcErrorCodes.parseError]: "parse-error",
  [rpcErrorCodes.invalidRequest]: "invalid-request",
  [rpcErrorCodes.methodNotFound]: "method-not-found",
  [rpcErrorCodes.invalidParams]: "invalid-params",
};

/** ear's message without its leading code: "" when it is only the code. */
export function trimCode(message: string, code: string): string {
  if (message === code) return "";
  if (message.startsWith(`${code} `)) return message.slice(code.length + 1);
  return message;
}

/**
 * A refusal in parts: ear's (an RpcError, its data.code or its JSON-RPC code in words, the JSON pointer of the param at
 * fault said once), a call the closed connection left unanswered, or one never sent because its JSON settings do not
 * parse (a local check, worded `invalid-json`, which ear never sends).
 */
export function refusalParts(error: unknown): RefusalParts {
  if (error instanceof RpcError) {
    const code = error.data?.code ?? rpcCodeWords[error.code] ?? String(error.code);
    let detail = trimCode(error.message, code);
    const pointer = error.data?.pointer;
    if (pointer !== undefined && !detail.includes(pointer)) {
      detail = detail === "" ? `at ${pointer}` : `${detail} at ${pointer}`;
    }
    return { verb: "refused", code, detail, severity: "error" };
  }
  if (error instanceof ConnectionClosedError) {
    const detail = "the connection to ear closed";
    return { verb: "had no answer", code: "not-connected", detail, severity: "warning" };
  }
  if (error instanceof Error) {
    return { verb: "not sent", code: "invalid-json", detail: error.message, severity: "error" };
  }
  return { verb: "not sent", code: "error", detail: String(error), severity: "error" };
}

/**
 * The toast for `asked` refused with `error`, about its route or node; a lost answer has no subject, so every call the
 * closed connection left unanswered folds into one toast.
 */
export function noticeOfRefusal(asked: Asked, error: unknown): NoticeInput {
  const parts = refusalParts(error);
  let subject: NoticeSubject | undefined;
  if (parts.code !== "not-connected") {
    if ("ends" in asked) subject = { kind: "route", ends: asked.ends };
    else if ("node" in asked) subject = { kind: "node", node: asked.node };
  }
  return { ...parts, what: describeAsked(asked), subject };
}

/**
 * The toast a node.state raises, if any: a change in place that could not finish, so the node restarts with the new
 * settings, which ear.conf holds ("fpp could not apply port in place, so it restarted: fell-back deadline 10s passed",
 * docs/editor.md#toasts). Only the stopping that begins the fallback raises it.
 */
export function noticeOfState(state: Notifications["node.state"]): NoticeInput | undefined {
  const restart = state.phase === "stopping" && state.cause?.kind === "restarting" ? state.cause.restart : undefined;
  if (restart?.kind !== "fell-back") return undefined;
  const keys = restart.keys !== undefined && restart.keys.length > 0 ? restart.keys.join(", ") : "its settings";
  return {
    severity: "warning",
    code: "fell-back",
    what: state.name,
    verb: `could not apply ${keys} in place, so it restarted`,
    detail: restart.detail ?? "",
    subject: { kind: "node", node: state.name },
  };
}

/** The refusals the drawer, the node box and the add form show in place, with no toast. */
export const placedCodes = ["file-changed", "invalid-text"] as const;

export function placedRefusal(error: unknown): boolean {
  const code = error instanceof RpcError ? error.data?.code : undefined;
  return code !== undefined && (placedCodes as readonly string[]).includes(code);
}

/**
 * What a toast's Show selects and which nodes it pans to: a node or route the graph holds, else null and no Show (a
 * toast naming what ear does not hold shows nothing, as an event naming it changes nothing).
 */
export function showOf(
  graph: Graph | null,
  subject: NoticeSubject | undefined,
): { selection: Selection; nodes: string[] } | null {
  if (graph === null || subject === undefined) return null;
  if (subject.kind === "node") {
    if (!graph.nodes.some((node) => node.name === subject.node)) return null;
    return { selection: { nodes: [subject.node], routes: [] }, nodes: [subject.node] };
  }
  const id = edgeId(subject.ends);
  if (!graph.routes.some((route) => edgeId(route) === id)) return null;
  return { selection: { nodes: [], routes: [id] }, nodes: [subject.ends.from.node, subject.ends.to.node] };
}
