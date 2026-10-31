// W1 criterion (2): the client with a fake socket. Ids are matched; errors reject with code and
// message; notifications are dispatched; after a close it reconnects, subscribes again (S6
// criterion 4: the graph, the graph file and the levels), and the new snapshot replaces the graph.
import { describe, expect, it } from "vitest";
import { Editor } from "./editor";
import type { Graph, JsonValue, ServerMessage } from "./protocol";
import { ConnectionClosedError, RpcClient, RpcError, firstRetryMs, longestRetryMs, type SocketEvents, type SocketLike } from "./rpc";
import { fixtures, messagesOf } from "./testFixtures";

class FakeSocket implements SocketLike {
  readonly sent: Array<{ id?: number; method: string; params?: unknown }> = [];
  closedByClient = false;

  constructor(private readonly events: SocketEvents) {}

  send(text: string): void {
    this.sent.push(JSON.parse(text));
  }
  close(): void {
    this.closedByClient = true;
  }
  /** The server accepts the connection. */
  accept(): void {
    this.events.open();
  }
  /** The server sends one text. */
  receive(message: unknown): void {
    this.events.message(JSON.stringify(message));
  }
  /** The connection drops. */
  drop(): void {
    this.events.close();
  }
  idOf(method: string): number {
    const request = this.sent.find((sent) => sent.method === method);
    if (request?.id === undefined) throw new Error(`no ${method} request was sent`);
    return request.id;
  }
}

/** A client on fake sockets, with the reconnect waits recorded and fired by the test. */
function harness() {
  const sockets: FakeSocket[] = [];
  const waits: Array<{ ms: number; retry: () => void }> = [];
  const messages: ServerMessage[] = [];
  const options = {
    url: "ws://ear/rpc",
    openSocket: (_url: string, events: SocketEvents) => {
      const socket = new FakeSocket(events);
      sockets.push(socket);
      return socket;
    },
    schedule: (retry: () => void, ms: number) => void waits.push({ ms, retry }),
  };
  return { sockets, waits, messages, options };
}

function connectedClient() {
  const h = harness();
  const client = new RpcClient({ ...h.options, onMessage: (message) => h.messages.push(message) });
  client.start();
  const socket = h.sockets[0]!;
  socket.accept();
  return { ...h, client, socket };
}

const graphOf = (graph: Graph): JsonValue => graph as unknown as JsonValue;

/** What every new connection asks for first. */
const subscriptions = ["graph.subscribe", "graphFile.subscribe", "levels.subscribe"];

describe("the rpc client", () => {
  it("subscribes to the graph, the graph file and the levels when the socket opens", () => {
    const { socket } = connectedClient();
    expect(socket.sent.map((request) => request.method)).toEqual(subscriptions);
    expect(socket.sent.every((request) => !("params" in request))).toBe(true);
  });

  it("matches each reply to its request by id, in any order", async () => {
    const { client, socket } = connectedClient();
    const add = client.call("node.add", { name: "n2", type: "white" });
    const remove = client.call("node.remove", { name: "noise" });
    const addRequest = socket.sent.find((request) => request.method === "node.add")!;
    expect(addRequest.params).toEqual({ name: "n2", type: "white" });
    socket.receive({ jsonrpc: "2.0", id: socket.idOf("node.remove"), result: { removed: 1 } });
    socket.receive({ jsonrpc: "2.0", id: socket.idOf("node.add"), result: { added: 1 } });
    await expect(add).resolves.toEqual({ added: 1 });
    await expect(remove).resolves.toEqual({ removed: 1 });
  });

  it("rejects a refused call with the error's code, message and data", async () => {
    const { client, socket } = connectedClient();
    const call = client.call("node.add", { name: "a.b", type: "white" });
    socket.receive({
      jsonrpc: "2.0",
      id: socket.idOf("node.add"),
      error: { code: -32000, message: "invalid-name a.b", data: { code: "invalid-name" } },
    });
    const error = await call.catch((caught: unknown) => caught);
    expect(error).toBeInstanceOf(RpcError);
    expect(error).toMatchObject({ code: -32000, message: "invalid-name a.b", data: { code: "invalid-name" } });
  });

  it("hands every server text to onMessage in order, notifications included", () => {
    const { socket, messages } = connectedClient();
    const sent = messagesOf(fixtures.edits.messages);
    for (const message of sent) socket.receive(message);
    expect(messages).toEqual(sent);
  });

  it("fails the calls in flight when the socket closes, and refuses calls until it reopens", async () => {
    const { client, socket } = connectedClient();
    const call = client.call("graph.get");
    socket.drop();
    await expect(call).rejects.toBeInstanceOf(ConnectionClosedError);
    await expect(client.call("graph.get")).rejects.toBeInstanceOf(ConnectionClosedError);
  });

  it("reconnects after a wait growing from 1 s to 30 s, and subscribes again", () => {
    const { sockets, waits } = connectedClient();
    const expected = [firstRetryMs, 2000, 4000, 8000, 16000, longestRetryMs, longestRetryMs];
    for (const ms of expected) {
      sockets.at(-1)!.drop();
      expect(waits.at(-1)!.ms).toBe(ms);
      const before = sockets.length;
      waits.at(-1)!.retry();
      expect(sockets.length).toBe(before + 1);
    }
    // An open socket resets the wait and subscribes again.
    const socket = sockets.at(-1)!;
    socket.accept();
    expect(socket.sent.map((request) => request.method)).toEqual(subscriptions);
    socket.drop();
    expect(waits.at(-1)!.ms).toBe(firstRetryMs);
  });

  it("stays closed after stop()", () => {
    const { client, socket, waits } = connectedClient();
    client.stop();
    expect(socket.closedByClient).toBe(true);
    socket.drop();
    expect(waits).toHaveLength(0);
  });
});

describe("the editor session", () => {
  it("replaces the graph with the new connection's snapshot after a reconnect", () => {
    const h = harness();
    const editor = new Editor(h.options);
    editor.client.start();
    const first = h.sockets[0]!;
    first.accept();
    expect(editor.connection()).toBe("open");
    // The edits fixture's snapshot, then n2 added before the connection drops.
    const edits = messagesOf(fixtures.edits.messages);
    first.receive({ ...edits[0]!, id: first.idOf("graph.subscribe") });
    first.receive(edits[1]);
    expect(editor.graph.get()!.nodes.map((node) => node.name)).toEqual(["n2", "noise", "rec"]);
    first.receive({ jsonrpc: "2.0", method: "levels", params: { nodes: { noise: { MONO: [0.5, 0.25] } } } });
    expect(editor.levels.get("noise", "MONO")).toEqual([0.5, 0.25]);

    first.drop();
    expect(editor.connection()).toBe("closed");
    expect(editor.levels.get("noise", "MONO")).toEqual([0, 0]);
    h.waits[0]!.retry();
    const second = h.sockets[1]!;
    second.accept();
    expect(second.sent.map((request) => request.method)).toEqual(subscriptions);
    second.receive({ jsonrpc: "2.0", id: second.idOf("graph.subscribe"), result: graphOf(fixtures.snapshot.graph) });
    expect(editor.graph.get()).toStrictEqual(fixtures.snapshot.graph);
  });

  it("holds the graph file from graphFile.subscribe's answer, and none when it is refused", async () => {
    const h = harness();
    const editor = new Editor(h.options);
    editor.client.start();
    const first = h.sockets[0]!;
    first.accept();
    first.receive({ jsonrpc: "2.0", id: first.idOf("graphFile.subscribe"), error: { code: -32601, message: "Method not found" } });
    expect(editor.file.get()).toBeNull();
    first.drop();
    h.waits[0]!.retry();
    const second = h.sockets[1]!;
    second.accept();
    const file = fixtures.graphFile.file!;
    second.receive({ jsonrpc: "2.0", id: second.idOf("graphFile.subscribe"), result: file as unknown as JsonValue });
    expect(editor.file.get()).toStrictEqual({ file, notices: [] });
  });
});
