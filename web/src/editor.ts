// One editor session: the client, the graph, the graph file, the levels and the counter moves (companions' and the
// diagnostics counters'), wired together outside React (docs/editor.md). Every server text goes to the graph's and the
// graph file's reducers, and each new graph to both moves before anything draws it; each levels notification replaces the level store; a closed
// connection makes every meter read silent until the new connection's levels arrive. The connection is kept as a view
// with the time of the next socket, for the top bar's pill (docs/editor.md#the-top-bar). The session also keeps the
// requests in flight (requests.ts, docs/editor.md#requests-in-flight) and the toast queue (notices.ts,
// docs/editor.md#toasts): ask() records a request until its reply, and makes a toast of any refusal its caller does
// not show in place. A closed connection clears the requests but no toast, so a reconnect drops nothing unseen.
import { CompanionMoves } from "./companions";
import { CounterMoves } from "./counters";
import { GraphStore } from "./graph";
import { GraphFileStore } from "./graphFile";
import { LevelStore } from "./levels";
import { noticeOfRefusal, noticeOfState, Notices } from "./notices";
import type { Methods, Notifications } from "./protocol";
import { askedOf, Requests, type Asked, type AskedMethod } from "./requests";
import { RpcClient, RpcError, type ConnectionStatus, type ParamsOf, type RpcClientOptions } from "./rpc";

/** The connection as the top bar's pill draws it. */
export interface ConnectionView {
  status: ConnectionStatus;
  /** On the browser's monotonic clock: when the next socket opens, while closed and a retry is scheduled. */
  retryAt?: number;
}

export interface AskOptions {
  /** The refusals the caller shows in place (notices.ts's placedRefusal); every other becomes a toast. */
  placed?: (error: unknown) => boolean;
}

export class Editor {
  readonly graph = new GraphStore();
  readonly file = new GraphFileStore();
  readonly levels = new LevelStore();
  readonly moves = new CompanionMoves();
  readonly counterMoves = new CounterMoves();
  readonly client: RpcClient;
  readonly requests: Requests;
  readonly notices: Notices;
  private view: ConnectionView = { status: "connecting" };
  private readonly statusListeners = new Set<() => void>();
  private readonly now: () => number;

  constructor(options: Omit<RpcClientOptions, "onMessage" | "onStatus" | "onRetry"> & { now?: () => number }) {
    const { now, ...clientOptions } = options;
    this.now = now ?? (() => performance.now());
    this.requests = new Requests(this.now);
    this.notices = new Notices(this.now);
    // The graph's first listeners, so a box drawn for a new graph reads that graph's moves.
    this.moves.attach(this.graph);
    this.counterMoves.attach(this.graph);
    this.client = new RpcClient({
      ...clientOptions,
      onMessage: (message) => {
        this.graph.apply(message);
        this.file.apply(message);
        if ("method" in message && message.method === "levels") {
          this.levels.replace((message.params as Notifications["levels"]).nodes);
        }
        if ("method" in message && message.method === "node.state") {
          const notice = noticeOfState(message.params as Notifications["node.state"]);
          if (notice !== undefined) this.notices.add(notice);
        }
      },
      onStatus: (status) => {
        if (status === "closed") {
          this.levels.clear();
          // The next snapshot is a first sighting: what moved while away did not move "in the last minute".
          this.moves.forget();
          this.counterMoves.forget();
          // The calls in flight had no answer; the client rejects each, and they are forgotten here at once.
          this.requests.clear();
        }
        this.view = { status };
        this.notify();
      },
      onRetry: (ms) => {
        this.view = { status: "closed", retryAt: this.now() + ms };
        this.notify();
      },
    });
  }

  /**
   * Sends `method`, kept as in flight until its reply, refusal or the connection's close; a failure the caller does
   * not show in place (options.placed) becomes a toast. The promise settles as the call does.
   */
  async ask<M extends AskedMethod>(
    method: M,
    params: Methods[M]["params"],
    options?: AskOptions,
  ): Promise<Methods[M]["result"]> {
    const asked = askedOf(method, params);
    const token = this.requests.begin(asked);
    try {
      const result = await this.client.call(method, ...([params] as ParamsOf<M>));
      this.requests.end(token, "answered");
      return result;
    } catch (error) {
      this.requests.end(token, error instanceof RpcError ? "refused" : "lost");
      if (options?.placed?.(error) !== true) this.report(asked, error);
      throw error;
    }
  }

  /** ask(), where nothing waits on the answer: a refusal is a toast and nothing more. */
  send<M extends AskedMethod>(method: M, params: Methods[M]["params"]): void {
    this.ask(method, params).catch(() => {});
  }

  /** A failure before or outside a call (settings that do not parse), as a toast. */
  report(asked: Asked, error: unknown): void {
    this.notices.add(noticeOfRefusal(asked, error));
  }

  connection(): ConnectionStatus {
    return this.view.status;
  }

  /** The same object until the connection changes, as useSyncExternalStore needs. */
  connectionView(): ConnectionView {
    return this.view;
  }

  subscribeConnection(listener: () => void): () => void {
    this.statusListeners.add(listener);
    return () => this.statusListeners.delete(listener);
  }

  private notify(): void {
    for (const listener of [...this.statusListeners]) listener();
  }
}
