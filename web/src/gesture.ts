// A fader's or knob's gesture, pure (docs/editor.md#gestures-in-the-editor): one state machine per control, a node and
// a live ramped decimal key, that turns presses, moves, releases, keys and the connection into gesture.begin, ticks
// (gesture.change notifications), gesture.end and gesture.cancel (docs/control-port.md#gestures), and ear's
// gesture.changed for its id into the control's state and a notice. The machines are kept per editor session, not per
// drawn control, so a gesture's id outlives the box or form that began it and a reconnect ends it by that id. Timers
// and animation frames come in from outside, so the tests move them by hand.
import type { NoticeInput } from "./notices";
import type { Graph, Notifications, ServerMessage } from "./protocol";
import { ConnectionClosedError, RpcError, type ConnectionStatus } from "./rpc";
import { valueText, type TaperRow } from "./taper";

/** gesture.begin's answer. */
export interface BeginAnswer {
  gesture: string;
  value: number;
  min: number;
  max: number;
  decimals: number;
}

/** What a gesture sends: begin, end and cancel are calls; a tick is a notification. */
export interface GestureClient {
  begin(name: string, key: string): Promise<BeginAnswer>;
  change(gesture: string, seq: number, value: number): void;
  end(gesture: string, value?: number): Promise<{ value: number }>;
  cancel(gesture: string): Promise<unknown>;
}

/** The clocks a gesture reads: a timer and an animation frame, each returning its cancel. */
export interface GestureTimers {
  after(ms: number, run: () => void): () => void;
  frame(run: () => void): () => void;
}

/** A begin with no answer yet shows as pending after this. */
export const pendingMs = 200;
/** While a hand holds still, the last tick again after this, so ear does not take the hand as gone (30 s). */
export const keepAliveMs = 10000;
/** A keyboard gesture ends this long after its last key. */
export const keyEndMs = 400;
/** A notice that needs the value ear's change made waits for the node's node.updated, at most this. */
export const noticeWaitMs = 1000;

export type GestureState = "idle" | "beginning" | "active" | "ending" | "aborted" | "frozen";

/** One control's state as it is drawn. */
export interface GestureView {
  state: GestureState;
  /** The value the control shows while not idle (idle: the node's, from the graph). */
  value?: number;
  /** A begin unanswered for pendingMs. */
  pending: boolean;
  /** The gesture's id, from its begin's answer until it closes (kept while frozen). */
  id?: string;
  /** The reach's range and places, from the begin's answer. */
  range?: { min: number; max: number; decimals: number };
  /** While frozen: the words under the control until the reconnect's end is answered. */
  lost?: string;
}

const idleView: GestureView = Object.freeze({ state: "idle", pending: false });

/** What a control needs of the session around it. */
interface ControlSide {
  client: GestureClient;
  timers: GestureTimers;
  notify(notice: NoticeInput): void;
  /** A notice whose words need ear's new value for the key: built once the node's change arrived. */
  notifyLater(node: string, build: () => NoticeInput): void;
  /** A not-live refusal of a row the schema calls live (stepped): send the value by node.set instead. */
  setInstead(name: string, key: string, value: number): void;
  /** The node's value of `key` in the graph now. */
  valueNow(name: string, key: string): number | undefined;
  /** The count row and its value now when `key` is a family member (key-removed's words). */
  countNow(name: string, key: string): { by: string; count: number } | undefined;
  idChanged(control: GestureControl, before: string | undefined): void;
}

type Intent = "hold" | "end" | "cancel";

export class GestureControl {
  private view: GestureView = idleView;
  private readonly listeners = new Set<() => void>();
  // Where the gesture is: what a reply finds must be the call it answers.
  private token = 0;
  private intent: Intent = "hold";
  private moved = false;
  private seq = 0;
  private lastSent: number | undefined;
  private committed: number | undefined;
  private pointerDown = false;
  private queued: number | undefined;
  private cancelPending: (() => void) | undefined;
  private cancelFrame: (() => void) | undefined;
  private cancelKeepAlive: (() => void) | undefined;
  private cancelKeyEnd: (() => void) | undefined;

  constructor(
    readonly name: string,
    readonly key: string,
    private row: TaperRow,
    private readonly side: ControlSide,
  ) {}

  /** The same object until a change, as useSyncExternalStore needs. */
  get(): GestureView {
    return this.view;
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  /** The row's facts may come again with a new schema. */
  setRow(row: TaperRow): void {
    this.row = row;
  }

  /** A pointer pressed on the control (or Alt-click and double-click, before their move and release). */
  press(): void {
    this.pointerDown = true;
    this.stopKeyEnd();
    this.hold();
  }

  /** The control moved to `value` (a pointer drag, a key, an assistive change). */
  move(value: number): void {
    if (this.view.state === "beginning") {
      this.moved = true;
      this.set({ ...this.view, value });
    } else if (this.view.state === "active") {
      this.moved = true;
      this.set({ ...this.view, value });
      this.cancelFrame ??= this.side.timers.frame(() => {
        this.cancelFrame = undefined;
        this.tick();
      });
    }
  }

  /** A key (or an assistive technology's change) moved it: begins on the first, ends keyEndMs after the last. */
  nudge(value: number): void {
    if (this.view.state === "idle") this.hold();
    this.move(value);
    this.stopKeyEnd();
    if (this.pointerDown) return;
    this.cancelKeyEnd = this.side.timers.after(keyEndMs, () => {
      this.cancelKeyEnd = undefined;
      if (!this.pointerDown) this.letGo();
    });
  }

  /** The pointer let go, its capture was lost or cancelled, or the control lost the focus. */
  release(): void {
    this.pointerDown = false;
    this.stopKeyEnd();
    this.letGo();
  }

  /**
   * One gesture that sets `value` and ends (double-click or Alt-click: the default). A gesture still ending sets it
   * once that end is answered.
   */
  setTo(value: number): void {
    if (this.view.state === "ending") {
      this.queued = value;
      return;
    }
    if (this.view.state === "idle") this.hold();
    this.move(value);
    this.pointerDown = false;
    this.stopKeyEnd();
    this.letGo();
  }

  /** Escape: cancels, the audio back to the committed value. */
  escape(): void {
    this.pointerDown = false;
    this.stopKeyEnd();
    switch (this.view.state) {
      case "beginning":
        this.intent = "cancel";
        return;
      case "active": {
        const id = this.view.id!;
        this.close();
        this.side.client.cancel(id).catch(() => {});
        return;
      }
      case "aborted":
        this.close();
        return;
      default:
        return;
    }
  }

  /** The connection closed or opened again. */
  connection(status: ConnectionStatus): void {
    const { state, id } = this.view;
    if (status === "open") {
      if (state !== "frozen" || id === undefined) return;
      this.ending(id, this.view.value, true);
      return;
    }
    if (status !== "closed") return;
    this.pointerDown = false;
    this.stopTimers();
    if ((state === "active" || state === "ending") && id !== undefined) {
      ++this.token;
      const lost = `Connection lost while you held ${this.name} ${this.key}; ear keeps the last value it heard.`;
      this.set({ ...this.view, state: "frozen", pending: false, lost });
    } else if (state !== "frozen") this.close();
  }

  /** gesture.changed for this control's id. */
  changed(params: Notifications["gesture.changed"]): void {
    const { state, id } = this.view;
    if (id === undefined || params.gesture !== id) return;
    if (params.phase === "aborted") {
      const reason = params.reason ?? "aborted";
      if (reason === "cancelled" && state !== "active" && state !== "frozen") return;
      // A refused end: its reply says why, so the notice is the reply's.
      if (reason === "refused" && state === "ending") return;
      const shown = this.view.value;
      this.abortNotice(reason, shown);
      ++this.token;
      this.stopTimers();
      if (this.pointerDown || this.cancelKeyEnd !== undefined) this.set({ state: "aborted", pending: false });
      else this.close();
      return;
    }
    // An end with a reason is ear's release; one with none while this control is not ending is another session's,
    // by the id (docs/editor.md#gestures-in-the-editor). This control's own end is closed by its reply.
    if (params.phase === "end" && (params.reason !== undefined || state !== "ending")) {
      ++this.token;
      this.stopTimers();
      this.endNotice(params.reason ?? "ended", params.value);
      this.close();
    }
  }

  /** A press or a first key: begins from idle; during a begin, holds on (a release before the answer is undone). */
  private hold(): void {
    switch (this.view.state) {
      case "idle":
        this.begin();
        return;
      case "beginning":
        this.intent = "hold";
        return;
      default:
        return;
    }
  }

  private letGo(): void {
    switch (this.view.state) {
      case "beginning":
        if (this.intent === "hold") this.intent = "end";
        return;
      case "active":
        this.ending(this.view.id!, this.moved ? this.view.value : undefined, false);
        return;
      case "aborted":
        this.close();
        return;
      default:
        return;
    }
  }

  private begin(): void {
    const token = ++this.token;
    this.intent = "hold";
    this.moved = false;
    this.seq = 0;
    this.lastSent = undefined;
    this.committed = this.side.valueNow(this.name, this.key);
    const value = this.committed;
    this.set(value !== undefined ? { state: "beginning", pending: false, value } : { state: "beginning", pending: false });
    this.cancelPending = this.side.timers.after(pendingMs, () => {
      this.cancelPending = undefined;
      if (this.token === token && this.view.state === "beginning") this.set({ ...this.view, pending: true });
    });
    this.side.client.begin(this.name, this.key).then(
      (answer) => {
        if (this.token !== token) {
          // A begin answered after its control let go (a lost connection): nothing holds it, so let ear keep it.
          this.side.client.cancel(answer.gesture).catch(() => {});
          return;
        }
        this.answered(answer);
      },
      (error: unknown) => {
        if (this.token !== token) return;
        this.stopTimers();
        const shown = this.view.value;
        const moved = this.moved;
        this.close();
        if (error instanceof ConnectionClosedError) return;
        if (error instanceof RpcError && error.data?.code === "not-live" && error.data.change === "live") {
          if (moved && shown !== undefined) this.side.setInstead(this.name, this.key, shown);
          return;
        }
        this.side.notify(this.beginRefusal(error));
      },
    );
  }

  private answered(answer: BeginAnswer): void {
    this.stopPending();
    this.committed = answer.value;
    const range = { min: answer.min, max: answer.max, decimals: answer.decimals };
    const value = this.moved && this.view.value !== undefined ? this.view.value : answer.value;
    const before = this.view.id;
    this.set({ state: "active", pending: false, value, id: answer.gesture, range });
    this.side.idChanged(this, before);
    if (this.intent === "cancel") {
      this.close();
      this.side.client.cancel(answer.gesture).catch(() => {});
      return;
    }
    if (this.intent === "end") {
      this.ending(answer.gesture, this.moved ? value : undefined, false);
      return;
    }
    if (this.moved) this.tick();
    else this.keepAlive();
  }

  /** Sends the shown value as the next tick, when it moved since the last. */
  private tick(): void {
    const { state, id, value } = this.view;
    if (state !== "active" || id === undefined || value === undefined) return;
    if (value !== this.lastSent) this.send(id, value);
  }

  private send(id: string, value: number): void {
    this.lastSent = value;
    this.side.client.change(id, ++this.seq, value);
    this.keepAlive();
  }

  private keepAlive(): void {
    this.cancelKeepAlive?.();
    this.cancelKeepAlive = this.side.timers.after(keepAliveMs, () => {
      this.cancelKeepAlive = undefined;
      const { state, id, value } = this.view;
      if (state === "active" && id !== undefined) this.send(id, this.lastSent ?? value ?? this.committed ?? 0);
    });
  }

  private ending(id: string, value: number | undefined, reconnected: boolean): void {
    const token = ++this.token;
    this.stopTimers();
    this.set({ ...this.view, state: "ending", pending: false });
    this.side.client.end(id, value).then(
      () => {
        if (this.token !== token) return;
        const queued = this.queued;
        this.close();
        if (queued !== undefined) this.setTo(queued);
      },
      (error: unknown) => {
        if (this.token !== token) return;
        if (error instanceof ConnectionClosedError) return; // frozen by the close already
        this.close();
        this.side.notify(this.endRefusal(error, reconnected));
      },
    );
  }

  private close(): void {
    ++this.token;
    this.queued = undefined;
    this.stopTimers();
    this.pointerDown = false;
    const before = this.view.id;
    this.set(idleView);
    if (before !== undefined) this.side.idChanged(this, before);
  }

  private stopPending(): void {
    this.cancelPending?.();
    this.cancelPending = undefined;
  }

  private stopKeyEnd(): void {
    this.cancelKeyEnd?.();
    this.cancelKeyEnd = undefined;
  }

  private stopTimers(): void {
    this.stopPending();
    this.stopKeyEnd();
    this.cancelFrame?.();
    this.cancelFrame = undefined;
    this.cancelKeepAlive?.();
    this.cancelKeepAlive = undefined;
  }

  private set(view: GestureView): void {
    this.view = view;
    for (const listener of [...this.listeners]) listener();
  }

  // ----- Notices (docs/editor.md#gestures-in-the-editor) -----

  private what(): string {
    return `${this.name} ${this.key}`;
  }

  private text(value: number | undefined): string {
    return value === undefined ? "its value" : valueText(this.row, value);
  }

  private beginRefusal(error: unknown): NoticeInput {
    const subject = { kind: "node" as const, node: this.name };
    if (!(error instanceof RpcError)) {
      return { severity: "error", code: "error", what: this.what(), verb: "could not be held", detail: String(error), subject };
    }
    const code = error.data?.code ?? String(error.code);
    let detail = error.message.startsWith(`${code} `) ? error.message.slice(code.length + 1) : error.message;
    if (code === "parameter-held") {
      const silentMs = error.data?.silentMs;
      detail =
        typeof silentMs === "number" && silentMs >= 1000
          ? `another editor holds it, silent for ${Math.round(silentMs / 1000)} s`
          : "another editor holds it";
    }
    return { severity: "warning", code, what: this.what(), verb: "could not be held", detail, subject };
  }

  private endRefusal(error: unknown, reconnected: boolean): NoticeInput {
    const subject = { kind: "node" as const, node: this.name };
    const code = error instanceof RpcError ? (error.data?.code ?? String(error.code)) : "error";
    if (reconnected && code === "unknown-gesture") {
      const now = this.side.valueNow(this.name, this.key);
      const detail = `ear had already let go of it, keeping ${this.text(now)}.`;
      return { severity: "info", code, what: this.what(), verb: "was let go after the connection was lost", detail, subject };
    }
    const message = error instanceof Error ? error.message : String(error);
    const detail = `${message.startsWith(`${code} `) ? message.slice(code.length + 1) : message}; it stays ${this.text(this.committed)}.`;
    return { severity: "error", code: "refused", what: this.what(), verb: `was not committed (${code})`, detail, subject };
  }

  private endNotice(reason: string, value: number | undefined): void {
    const subject = { kind: "node" as const, node: this.name };
    const kept = this.text(value);
    const detail =
      reason === "stale"
        ? `ear heard nothing about ${this.what()} for 30 s and kept ${kept}.`
        : reason === "session-closed"
          ? `Connection lost while you held ${this.what()}; ear kept ${kept}.`
          : reason === "ended"
            ? `Another connection ended it; ear kept ${kept}.`
            : `ear ended it and kept ${kept}.`;
    this.side.notify({ severity: "info", code: reason, what: this.what(), verb: "was let go", detail, subject });
  }

  private abortNotice(reason: string, shown: number | undefined): void {
    const subject = { kind: "node" as const, node: this.name };
    const what = this.what();
    const base = { severity: "warning" as const, code: reason, what, verb: "was let go", subject };
    const again =
      shown !== undefined
        ? { label: `Set ${this.text(shown)} again`, name: this.name, settings: { [this.key]: shown } }
        : undefined;
    const withAgain = (notice: NoticeInput): NoticeInput => (again !== undefined ? { ...notice, again } : notice);
    switch (reason) {
      case "hand-edit":
      case "file-saved": {
        const how = reason === "hand-edit" ? "was edited by hand" : "was saved from the text pane";
        this.side.notifyLater(this.name, () =>
          withAgain({
            ...base,
            detail: `ear.conf ${how} and changed ${what} while you held it; it now says ${this.text(this.side.valueNow(this.name, this.key))}.`,
          }),
        );
        return;
      }
      case "edited":
        this.side.notifyLater(this.name, () =>
          withAgain({ ...base, detail: `Another editor changed what shares ${what} while you held it.` }),
        );
        return;
      case "key-removed":
        this.side.notifyLater(this.name, () => {
          const count = this.side.countNow(this.name, this.key);
          const now = count !== undefined ? `${this.name}'s ${count.by} is now ${count.count}, so ` : "";
          return { ...base, detail: `${now}${this.key}, which you held, is not used.` };
        });
        return;
      case "node-removed":
        this.side.notify({ ...base, detail: `${this.name} was removed while you held ${this.key}.` });
        return;
      case "cancelled":
        this.side.notify({ ...base, detail: `Another editor cancelled it; it stays ${this.text(this.committed)}.` });
        return;
      case "refused":
        this.side.notify({ ...base, severity: "error", detail: `ear refused its end; it stays ${this.text(this.committed)}.` });
        return;
      case "crossed":
        this.side.notify({
          ...base,
          detail: `Another change left it outside what ${this.name} allows beside its other values; it stays ${this.text(this.committed)}.`,
        });
        return;
      default:
        this.side.notify({ ...base, detail: `ear stopped it (${reason}).` });
    }
  }
}

/** Where the session's controls read the graph: the node's value of a key, and a family member's count. */
export interface GraphReader {
  graph(): Graph | null;
  /** The count row of `key`'s family, when `key` is a member of one (from the node's type's schema). */
  countRowOf(name: string, key: string): { by: string; default: number } | undefined;
}

/**
 * Every control of one editor session, by node and key: created when first drawn and kept while a gesture of it is
 * open, so its id outlives what drew it. Messages and the connection reach every control; a notice needing ear's new
 * value waits for the node's node.updated (at most noticeWaitMs).
 */
export class GestureControls {
  private readonly controls = new Map<string, GestureControl>();
  private readonly byId = new Map<string, GestureControl>();
  private readonly later: Array<{ node: string; build: () => NoticeInput; cancel: () => void }> = [];
  private readonly side: ControlSide;

  constructor(
    client: GestureClient,
    private readonly timers: GestureTimers,
    private readonly reader: GraphReader,
    private readonly notify: (notice: NoticeInput) => void,
    setInstead: (name: string, key: string, value: number) => void,
  ) {
    this.side = {
      client,
      timers,
      notify,
      notifyLater: (node, build) => this.notifyLater(node, build),
      setInstead,
      valueNow: (name, key) => {
        const node = this.reader.graph()?.nodes.find((one) => one.name === name);
        const value = node?.settings[key];
        return typeof value === "number" ? value : undefined;
      },
      countNow: (name, key) => {
        const row = this.reader.countRowOf(name, key);
        if (row === undefined) return undefined;
        const node = this.reader.graph()?.nodes.find((one) => one.name === name);
        const value = node?.settings[row.by];
        return { by: row.by, count: typeof value === "number" ? value : row.default };
      },
      idChanged: (control, before) => {
        if (before !== undefined && this.byId.get(before) === control) this.byId.delete(before);
        const id = control.get().id;
        if (id !== undefined) this.byId.set(id, control);
      },
    };
  }

  /** The control of `name`'s `key`, made on first use; `row` its range, taper, unit and places. */
  control(name: string, key: string, row: TaperRow): GestureControl {
    const id = `${name}\n${key}`;
    let control = this.controls.get(id);
    if (control === undefined) {
      control = new GestureControl(name, key, row, this.side);
      this.controls.set(id, control);
    } else control.setRow(row);
    return control;
  }

  /** The control holding gesture `id`, if this session holds it. */
  holding(id: string): GestureControl | undefined {
    return this.byId.get(id);
  }

  /** Every server message, after the graph took it. */
  apply(message: ServerMessage): void {
    if (!("method" in message)) return;
    if (message.method === "gesture.changed") {
      const params = message.params as Notifications["gesture.changed"];
      this.byId.get(params.gesture)?.changed(params);
      return;
    }
    if (message.method === "node.updated" || message.method === "node.removed") {
      const name = (message.params as { name: string }).name;
      this.flush((one) => one.node === name);
    }
  }

  connection(status: ConnectionStatus): void {
    for (const control of [...this.controls.values()]) control.connection(status);
  }

  /** The window lost the focus or was hidden: every control lets go. */
  releaseAll(): void {
    for (const control of [...this.controls.values()]) control.release();
  }

  private notifyLater(node: string, build: () => NoticeInput): void {
    const entry = { node, build, cancel: () => {} };
    entry.cancel = this.timers.after(noticeWaitMs, () => this.flush((one) => one === entry));
    this.later.push(entry);
  }

  private flush(which: (entry: { node: string; build: () => NoticeInput }) => boolean): void {
    for (let i = 0; i < this.later.length; ) {
      const entry = this.later[i]!;
      if (!which(entry)) {
        ++i;
        continue;
      }
      this.later.splice(i, 1);
      entry.cancel();
      this.notify(entry.build());
    }
  }
}
