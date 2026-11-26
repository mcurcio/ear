// The editor session on fake sockets and a fake clock: its connection view (docs/editor.md#the-top-bar), the status
// and while closed when the next socket opens, the same object between changes; and its ask(), a request in flight
// until its reply or the connection's close, a refusal not shown in place made a toast (docs/editor.md#feedback).
import { describe, expect, it } from "vitest";
import { Editor } from "./editor";
import { placedRefusal } from "./notices";
import { ghostWires } from "./requests";
import { ConnectionClosedError, firstRetryMs, RpcError, type SocketEvents } from "./rpc";
import { FakeSocket } from "./testFixtures";

/** An editor on fake sockets and a fake clock, its first socket accepted. */
function session() {
  const clock = { now: 0 };
  const sockets: FakeSocket[] = [];
  const waits: Array<() => void> = [];
  const editor = new Editor({
    url: "ws://ear/rpc",
    now: () => clock.now,
    openSocket: (_url: string, events: SocketEvents) => {
      const socket = new FakeSocket(events);
      sockets.push(socket);
      return socket;
    },
    schedule: (retry) => void waits.push(retry),
  });
  editor.client.start();
  sockets[0]!.accept();
  return { editor, clock, sockets, waits };
}

const ends = { from: { node: "noise", port: "noise.mono" }, to: { node: "rec", port: "FX" } };

describe("the connection view", () => {
  it("follows the socket and says when the next one opens", () => {
    let clock = 5000;
    const sockets: FakeSocket[] = [];
    const waits: Array<() => void> = [];
    const editor = new Editor({
      url: "ws://ear/rpc",
      now: () => clock,
      openSocket: (_url: string, events: SocketEvents) => {
        const socket = new FakeSocket(events);
        sockets.push(socket);
        return socket;
      },
      schedule: (retry) => void waits.push(retry),
    });
    let told = 0;
    editor.subscribeConnection(() => told++);
    expect(editor.connectionView()).toStrictEqual({ status: "connecting" });

    editor.client.start();
    const connecting = editor.connectionView();
    expect(connecting).toStrictEqual({ status: "connecting" });
    expect(editor.connectionView()).toBe(connecting);

    sockets[0]!.accept();
    const open = editor.connectionView();
    expect(open).toStrictEqual({ status: "open" });
    expect(editor.connectionView()).toBe(open);
    expect(editor.connection()).toBe("open");

    clock = 7000;
    sockets[0]!.drop();
    const closed = editor.connectionView();
    expect(closed).toStrictEqual({ status: "closed", retryAt: 7000 + firstRetryMs });
    expect(editor.connectionView()).toBe(closed);
    expect(editor.connection()).toBe("closed");
    expect(told).toBeGreaterThanOrEqual(4);

    waits[0]!();
    expect(editor.connectionView()).toStrictEqual({ status: "connecting" });
    expect(editor.connectionView()).not.toHaveProperty("retryAt");
  });
});

describe("ask", () => {
  it("is in flight until ear answers", async () => {
    const { editor, sockets } = session();
    const asked = editor.ask("node.start", { name: "porch" });
    expect(editor.requests.get().inFlight.map((request) => request.asked)).toStrictEqual([
      { method: "node.start", node: "porch" },
    ]);
    sockets[0]!.receive({ jsonrpc: "2.0", id: sockets[0]!.idOf("node.start"), result: { applied: "start" } });
    await expect(asked).resolves.toStrictEqual({ applied: "start" });
    expect(editor.requests.get().inFlight).toEqual([]);
    expect(editor.notices.get().notices).toEqual([]);
  });

  it("makes a refused route request's ghost refused and one toast naming the route", async () => {
    const { editor, clock, sockets } = session();
    clock.now = 500;
    const asked = editor.ask("route.add", ends);
    expect(ghostWires(editor.requests.get(), [])).toMatchObject([{ ends, refused: false }]);
    sockets[0]!.receive({
      jsonrpc: "2.0",
      id: sockets[0]!.idOf("route.add"),
      error: { code: -32000, message: "unknown-port rec:FX", data: { code: "unknown-port" } },
    });
    await expect(asked).rejects.toBeInstanceOf(RpcError);
    expect(editor.requests.get().inFlight).toEqual([]);
    expect(ghostWires(editor.requests.get(), [])).toMatchObject([{ ends, refused: true }]);
    expect(editor.requests.nextChange()).toBe(1500);
    const notices = editor.notices.get().notices;
    expect(notices).toHaveLength(1);
    expect(notices[0]).toMatchObject({
      severity: "error",
      what: "route noise:noise.mono → rec:FX",
      verb: "refused",
      code: "unknown-port",
      detail: "rec:FX",
    });
  });

  it("forgets the requests at once when the socket drops, and keeps the toasts across the reconnect", async () => {
    const { editor, sockets, waits } = session();
    editor.report({ method: "node.update", node: "porch" }, new Error("settings are not JSON"));
    const asked = editor.ask("graphFile.set", { text: "", version: "v1" });
    editor.send("route.add", ends);
    expect(editor.requests.get().inFlight).toHaveLength(2);

    sockets[0]!.drop();
    expect(editor.requests.get().inFlight).toEqual([]);
    await expect(asked).rejects.toBeInstanceOf(ConnectionClosedError);
    await Promise.resolve();
    expect(ghostWires(editor.requests.get(), [])).toEqual([]);
    const lost = editor.notices.get().notices.filter((notice) => notice.code === "not-connected");
    expect(lost).toHaveLength(1);
    expect(lost[0]!.count).toBe(2);

    waits[0]!();
    sockets[1]!.accept();
    expect(editor.notices.get().notices.map((notice) => notice.code)).toEqual(["invalid-json", "not-connected"]);
  });

  it("leaves a refusal the caller places to it, and makes a toast of any other", async () => {
    const { editor, sockets } = session();
    const invalid = editor.ask("graphFile.set", { text: "x", version: "v1" }, { placed: placedRefusal });
    sockets[0]!.receive({
      jsonrpc: "2.0",
      id: sockets[0]!.idOf("graphFile.set"),
      error: { code: -32000, message: "invalid-text", data: { code: "invalid-text", diagnostics: [] } },
    });
    await expect(invalid).rejects.toBeInstanceOf(RpcError);
    expect(editor.notices.get().notices).toEqual([]);

    const unknown = editor.ask("node.setText", { name: "ghost", text: "", version: "v1" }, { placed: placedRefusal });
    sockets[0]!.receive({
      jsonrpc: "2.0",
      id: sockets[0]!.idOf("node.setText"),
      error: { code: -32000, message: "unknown-node ghost", data: { code: "unknown-node" } },
    });
    await expect(unknown).rejects.toBeInstanceOf(RpcError);
    expect(editor.notices.get().notices.map((notice) => notice.code)).toEqual(["unknown-node"]);
  });

  it("never rejects through send", async () => {
    const { editor, sockets } = session();
    editor.send("node.remove", { name: "ghost" });
    sockets[0]!.receive({
      jsonrpc: "2.0",
      id: sockets[0]!.idOf("node.remove"),
      error: { code: -32000, message: "unknown-node ghost", data: { code: "unknown-node" } },
    });
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(editor.notices.get().notices.map((notice) => notice.what)).toEqual(["remove ghost"]);
    expect(editor.requests.get().inFlight).toEqual([]);
  });
});

describe("the type schemas", () => {
  it("are asked once for the hash a Graph names, and held once ear answers", async () => {
    const { editor, sockets } = session();
    const socket = sockets[0]!;
    const graph = {
      protocol: 1,
      features: ["types"],
      typeSchemaHash: "fnv1a64:0123456789abcdef",
      graphFrequency: 48000,
      blockSize: 256,
      realtime: false,
      scheduling: "SCHED_OTHER",
      types: ["white"],
      nodes: [],
      routes: [],
    };
    const subscribe = socket.sent.find((request) => request.method === "graph.subscribe")!;
    socket.receive({ jsonrpc: "2.0", id: subscribe.id, result: graph });
    socket.receive({ jsonrpc: "2.0", method: "route.added", params: ends }); // another text: no second ask
    const asked = socket.sent.filter((request) => request.method === "types.get");
    expect(asked).toHaveLength(1);
    expect(asked[0]!.params).toStrictEqual({});
    const white = { name: "white", settings: [], inputs: [], outputs: ["noise"], reconfigureMs: 10000 };
    socket.receive({
      jsonrpc: "2.0",
      id: asked[0]!.id,
      result: { protocol: 1, hash: graph.typeSchemaHash, typeSchemas: [white] },
    });
    // Held once the store is told: its listener is the event waited for.
    await new Promise<void>((resolve) => {
      if (editor.types.get() !== null) resolve();
      else editor.types.subscribe(resolve);
    });
    expect(editor.types.schemaOf(editor.graph.get(), "white")).toStrictEqual(white);
  });
});
