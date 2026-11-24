// The toast queue, pure (docs/editor.md#toasts): a notice is added, the same code for the same subject within 5 s
// counts up instead of stacking, at most four show with the older folded into "and n more", and each lasts 8 s on the
// browser's monotonic clock (as marks.ts times its minute), none expiring while the toasts are held (hovered or
// focused). A closed connection clears nothing here, so a reconnect drops nothing the person has not seen. Also here:
// a refusal in words (`route noise:noise.mono → rec:FX refused: unknown-port rec:FX`), which refusals a form shows in
// place, and what a toast's Show selects. Slice 5's node events are designed to feed this same queue by its rule
// (docs/editor.md#toasts), so NoticeInput is plain words, a severity and a subject.
import { edgeId } from "./connect";
import { rpcErrorCodes, type Graph, type RouteEnds } from "./protocol";
import { describeAsked, type Asked } from "./requests";
import { ConnectionClosedError, RpcError } from "./rpc";
import type { Selection } from "./selection";

/** How long a toast lasts unheld. */
export const noticeMs = 8000;
/** The same code for the same subject within this counts up. */
export const coalesceMs = 5000;
/** The most toasts shown at once. */
export const shownNoticeCap = 4;

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

export interface Notice extends NoticeInput {
  id: number;
  /** How many times it was added within coalesceMs of the last. */
  count: number;
  lastAt: number;
  expiresAt: number;
}

export interface NoticesView {
  /** Oldest first. */
  notices: readonly Notice[];
  /** While held: since when. */
  heldSince?: number;
}

const noNotices: NoticesView = Object.freeze({ notices: Object.freeze([]) });

/** The key two notices coalesce by: the code and the subject. */
export function noticeKey(code: string, subject?: NoticeSubject): string {
  if (subject === undefined) return `${code}\n`;
  return subject.kind === "node" ? `${code}\nnode ${subject.node}` : `${code}\nroute ${edgeId(subject.ends)}`;
}

export class Notices {
  private view: NoticesView = noNotices;
  private next = 1;
  private readonly listeners = new Set<() => void>();

  constructor(private readonly now: () => number = () => performance.now()) {}

  add(input: NoticeInput): void {
    const now = this.now();
    const key = noticeKey(input.code, input.subject);
    const same = this.view.notices.find(
      (notice) => noticeKey(notice.code, notice.subject) === key && now - notice.lastAt < coalesceMs,
    );
    const rest = same === undefined ? this.view.notices : this.view.notices.filter((notice) => notice !== same);
    const added: Notice =
      same === undefined
        ? { ...input, id: this.next++, count: 1, lastAt: now, expiresAt: now + noticeMs }
        : { ...same, ...input, count: same.count + 1, lastAt: now, expiresAt: now + noticeMs };
    this.set({ ...this.view, notices: [...rest, added] });
  }

  dismiss(id: number): void {
    const notices = this.view.notices.filter((notice) => notice.id !== id);
    if (notices.length !== this.view.notices.length) this.set({ ...this.view, notices });
  }

  /** Held (hovered or focused), no toast expires; let go, each expiry moves on by the time held. */
  hold(held: boolean): void {
    const { heldSince } = this.view;
    if (held) {
      if (heldSince === undefined) this.set({ ...this.view, heldSince: this.now() });
      return;
    }
    if (heldSince === undefined) return;
    const by = this.now() - heldSince;
    this.set({ notices: this.view.notices.map((notice) => ({ ...notice, expiresAt: notice.expiresAt + by })) });
  }

  /** Drops the toasts whose time is over; nothing while held. */
  expire(): void {
    if (this.view.heldSince !== undefined) return;
    const now = this.now();
    const notices = this.view.notices.filter((notice) => notice.expiresAt > now);
    if (notices.length !== this.view.notices.length) this.set({ ...this.view, notices });
  }

  /** When the soonest toast expires; undefined while held or with none. */
  nextExpiry(): number | undefined {
    if (this.view.heldSince !== undefined) return undefined;
    let soonest: number | undefined;
    for (const { expiresAt } of this.view.notices) {
      if (soonest === undefined || expiresAt < soonest) soonest = expiresAt;
    }
    return soonest;
  }

  /** The same object until a change, as useSyncExternalStore needs. */
  get(): NoticesView {
    return this.view;
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private set(view: NoticesView): void {
    this.view = view;
    for (const listener of [...this.listeners]) listener();
  }
}

/** The newest shownNoticeCap toasts, oldest first, and how many older ones fold into "and n more". */
export function shownNotices(view: NoticesView): { shown: readonly Notice[]; more: number } {
  const more = Math.max(0, view.notices.length - shownNoticeCap);
  return { shown: view.notices.slice(more), more };
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
