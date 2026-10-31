// The control port's client: JSON-RPC 2.0 over one WebSocket (docs/design/batch2.md sections 6
// and 8). It numbers requests, matches each reply to its request, hands every server text to
// onMessage in arrival order, and when the socket opens it subscribes to the graph, to the graph
// file and to the levels. When the socket closes it fails the calls in flight and opens a new socket after a
// growing wait (1 s doubling to 30 s): a stated retry rate, not a schedule. The new socket
// subscribes again, so the client starts over from a fresh snapshot (list, then watch).
import type { MethodName, Methods, RpcErrorObject, ServerMessage } from "./protocol";

/** One open socket, as the client uses it. */
export interface SocketLike {
  send(text: string): void;
  close(): void;
}

/** What a socket tells the client: it opened, a text arrived, it closed (for any reason). */
export interface SocketEvents {
  open(): void;
  message(text: string): void;
  close(): void;
}

/** Opens the browser's WebSocket to `url` and forwards its events. */
export function openWebSocket(url: string, events: SocketEvents): SocketLike {
  const socket = new WebSocket(url);
  socket.addEventListener("open", () => events.open());
  socket.addEventListener("message", (event) => {
    if (typeof event.data === "string") events.message(event.data);
  });
  socket.addEventListener("close", () => events.close());
  return socket;
}

export type ConnectionStatus = "connecting" | "open" | "closed";

export interface RpcClientOptions {
  url: string;
  /** Every server text, in the order it arrived, before the call it answers settles. */
  onMessage: (message: ServerMessage) => void;
  onStatus?: (status: ConnectionStatus) => void;
  openSocket?: (url: string, events: SocketEvents) => SocketLike;
  /** Runs `retry` once after `ms` milliseconds (the reconnect wait). */
  schedule?: (retry: () => void, ms: number) => void;
}

export const firstRetryMs = 1000;
export const longestRetryMs = 30000;

/** A request the server refused: the JSON-RPC error's code and message, and data.code if any. */
export class RpcError extends Error {
  readonly code: number;
  readonly data: RpcErrorObject["data"];

  constructor(error: RpcErrorObject) {
    super(error.message);
    this.name = "RpcError";
    this.code = error.code;
    this.data = error.data;
  }
}

/** A call that had no answer because the connection closed (or was not open). */
export class ConnectionClosedError extends Error {
  constructor() {
    super("not connected to ear");
    this.name = "ConnectionClosedError";
  }
}

/** A method's params as call()'s rest arguments: none when the method takes none. */
export type ParamsOf<M extends MethodName> = Methods[M]["params"] extends undefined ? [] : [Methods[M]["params"]];

interface Pending {
  resolve: (result: unknown) => void;
  reject: (error: Error) => void;
}

export class RpcClient {
  private readonly options: Required<RpcClientOptions>;
  private socket: SocketLike | null = null;
  private open = false;
  private stopped = true;
  private nextId = 1;
  private retryMs = firstRetryMs;
  private readonly pending = new Map<number, Pending>();

  constructor(options: RpcClientOptions) {
    this.options = {
      onStatus: () => {},
      openSocket: openWebSocket,
      schedule: (retry, ms) => void setTimeout(retry, ms),
      ...options,
    };
  }

  start(): void {
    if (!this.stopped) return;
    this.stopped = false;
    this.connect();
  }

  /** Closes the socket for good: no reconnect. */
  stop(): void {
    this.stopped = true;
    this.socket?.close();
    this.closed();
  }

  call<M extends MethodName>(method: M, ...params: ParamsOf<M>): Promise<Methods[M]["result"]> {
    const socket = this.socket;
    if (!this.open || socket === null) return Promise.reject(new ConnectionClosedError());
    const id = this.nextId++;
    const request = params.length === 0 ? { jsonrpc: "2.0", id, method } : { jsonrpc: "2.0", id, method, params: params[0] };
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve: resolve as (result: unknown) => void, reject });
      socket.send(JSON.stringify(request));
    });
  }

  private connect(): void {
    this.options.onStatus("connecting");
    // Events from a socket this client has let go of are ignored.
    let socket: SocketLike | null = null;
    socket = this.options.openSocket(this.options.url, {
      open: () => {
        if (this.socket !== socket) return;
        this.open = true;
        this.retryMs = firstRetryMs;
        this.options.onStatus("open");
        // The results reach onMessage like every other text; a failure surfaces as the close.
        this.call("graph.subscribe").catch(() => {});
        // An error answer (an ear without the graph file) leaves the file pane hidden.
        this.call("graphFile.subscribe").catch(() => {});
        this.call("levels.subscribe").catch(() => {});
      },
      message: (text) => {
        if (this.socket === socket) this.receive(text);
      },
      close: () => {
        if (this.socket !== socket) return;
        this.closed();
        if (this.stopped) return;
        const wait = this.retryMs;
        this.retryMs = Math.min(this.retryMs * 2, longestRetryMs);
        this.options.schedule(() => {
          if (!this.stopped && this.socket === null) this.connect();
        }, wait);
      },
    });
    this.socket = socket;
  }

  private closed(): void {
    const wasConnected = this.socket !== null;
    this.socket = null;
    this.open = false;
    const failed = [...this.pending.values()];
    this.pending.clear();
    for (const call of failed) call.reject(new ConnectionClosedError());
    if (wasConnected) this.options.onStatus("closed");
  }

  private receive(text: string): void {
    let parsed: unknown;
    try {
      parsed = JSON.parse(text);
    } catch {
      return; // the server sends JSON only; a broken text has nothing to apply
    }
    for (const message of Array.isArray(parsed) ? (parsed as unknown[]) : [parsed]) {
      if (typeof message !== "object" || message === null) continue;
      this.options.onMessage(message as ServerMessage);
      if (!("id" in message) || typeof message.id !== "number") continue;
      const call = this.pending.get(message.id);
      if (call === undefined) continue;
      this.pending.delete(message.id);
      if ("error" in message) call.reject(new RpcError(message.error as RpcErrorObject));
      else call.resolve("result" in message ? message.result : undefined);
    }
  }
}
