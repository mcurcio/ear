// One editor session: the client, the graph, the graph file and the levels, wired together
// outside React (docs/design/batch2.md section 8). Every server text goes to the graph's and the
// graph file's reducers; each levels notification replaces the level store; a closed connection
// makes every meter read silent until the new connection's levels arrive.
import { GraphStore } from "./graph";
import { GraphFileStore } from "./graphFile";
import { LevelStore } from "./levels";
import type { Notifications } from "./protocol";
import { RpcClient, type ConnectionStatus, type RpcClientOptions } from "./rpc";

export class Editor {
  readonly graph = new GraphStore();
  readonly file = new GraphFileStore();
  readonly levels = new LevelStore();
  readonly client: RpcClient;
  private status: ConnectionStatus = "connecting";
  private readonly statusListeners = new Set<() => void>();

  constructor(options: Omit<RpcClientOptions, "onMessage" | "onStatus">) {
    this.client = new RpcClient({
      ...options,
      onMessage: (message) => {
        this.graph.apply(message);
        this.file.apply(message);
        if ("method" in message && message.method === "levels") {
          this.levels.replace((message.params as Notifications["levels"]).nodes);
        }
      },
      onStatus: (status) => {
        if (status === "closed") this.levels.clear();
        this.status = status;
        for (const listener of [...this.statusListeners]) listener();
      },
    });
  }

  connection(): ConnectionStatus {
    return this.status;
  }

  subscribeConnection(listener: () => void): () => void {
    this.statusListeners.add(listener);
    return () => this.statusListeners.delete(listener);
  }
}
