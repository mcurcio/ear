// One editor session: the client, the graph, the graph file, the levels and the counter moves (companions' and the
// diagnostics counters'), wired together outside React (docs/editor.md). Every server text goes to the graph's and the
// graph file's reducers, and each new graph to both moves before anything draws it; each levels notification replaces the level store; a closed
// connection makes every meter read silent until the new connection's levels arrive. The connection is kept as a view
// with the time of the next socket, for the top bar's pill (docs/editor.md#the-top-bar).
import { CompanionMoves } from "./companions";
import { CounterMoves } from "./counters";
import { GraphStore } from "./graph";
import { GraphFileStore } from "./graphFile";
import { LevelStore } from "./levels";
import type { Notifications } from "./protocol";
import { RpcClient, type ConnectionStatus, type RpcClientOptions } from "./rpc";

/** The connection as the top bar's pill draws it. */
export interface ConnectionView {
  status: ConnectionStatus;
  /** On the browser's monotonic clock: when the next socket opens, while closed and a retry is scheduled. */
  retryAt?: number;
}

export class Editor {
  readonly graph = new GraphStore();
  readonly file = new GraphFileStore();
  readonly levels = new LevelStore();
  readonly moves = new CompanionMoves();
  readonly counterMoves = new CounterMoves();
  readonly client: RpcClient;
  private view: ConnectionView = { status: "connecting" };
  private readonly statusListeners = new Set<() => void>();
  private readonly now: () => number;

  constructor(options: Omit<RpcClientOptions, "onMessage" | "onStatus" | "onRetry"> & { now?: () => number }) {
    const { now, ...clientOptions } = options;
    this.now = now ?? (() => performance.now());
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
      },
      onStatus: (status) => {
        if (status === "closed") {
          this.levels.clear();
          // The next snapshot is a first sighting: what moved while away did not move "in the last minute".
          this.moves.forget();
          this.counterMoves.forget();
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
