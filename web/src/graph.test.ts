// The reducer turns every fixture's messages into its graph exactly; events
// naming unknown nodes are ignored; a snapshot replaces the state.
import { describe, expect, it } from "vitest";
import { GraphStore, rateLabel, reduce, type GraphState } from "./graph";
import type { Graph, JsonValue, ServerMessage } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";

function replay(messages: ServerMessage[], start: GraphState = null): GraphState {
  return messages.reduce<GraphState>(reduce, start);
}

const editsMessages = messagesOf(fixtures.edits.messages);
const snapshotMessage = editsMessages[0]!;

describe("the graph reducer", () => {
  for (const [name, fixture] of Object.entries(fixtures)) {
    it(`turns the ${name} fixture's messages into its graph`, () => {
      expect(replay(messagesOf(fixture.messages))).toStrictEqual(fixture.graph);
    });
  }

  it("ignores events before the first snapshot", () => {
    expect(replay(editsMessages.slice(1))).toBeNull();
  });

  it("ignores events naming a node or route it does not know", () => {
    const start = replay([snapshotMessage]);
    const ghostly: ServerMessage[] = [
      { jsonrpc: "2.0", method: "node.state", params: { name: "ghost", state: "running" } },
      { jsonrpc: "2.0", method: "node.updated", params: { name: "ghost", settings: { a: 1 } } },
      { jsonrpc: "2.0", method: "node.removed", params: { name: "ghost" } },
      { jsonrpc: "2.0", method: "node.stopped", params: { name: "ghost", droppedFrames: 3 } },
      {
        jsonrpc: "2.0",
        method: "route.added",
        params: { from: { node: "ghost", port: "noise.mono" }, to: { node: "rec", port: "record.right" } },
      },
      {
        jsonrpc: "2.0",
        method: "route.status",
        params: { from: { node: "noise", port: "noise.mono" }, to: { node: "rec", port: "record.right" }, status: "linked" },
      },
      {
        jsonrpc: "2.0",
        method: "route.removed",
        params: { from: { node: "noise", port: "noise.mono" }, to: { node: "rec", port: "record.right" } },
      },
      {
        jsonrpc: "2.0",
        method: "node.companionsChanged",
        params: { name: "ghost", companions: [{ name: "ghost.rtp", kind: "rtp-receiver", bank: "from_rtp", ready: true }] },
      },
    ];
    expect(replay(ghostly, start)).toBe(start);
  });

  it("replaces a node's whole companion list, one held without a list (an ear without the feature) included", () => {
    const start = replay([snapshotMessage]);
    const { companions: _none, ...withoutList } = start!.nodes.find((node) => node.name === "rec")!;
    const older = { ...start!, nodes: start!.nodes.map((node) => (node.name === "rec" ? withoutList : node)) };
    const sink = { name: "rec.writer", kind: "sink", bank: "writer", ready: true, dropped: 4096 };
    const told: ServerMessage = {
      jsonrpc: "2.0",
      method: "node.companionsChanged",
      params: { name: "rec", companions: [sink] },
    };
    expect(replay([told], older)!.nodes.find((node) => node.name === "rec")!.companions).toStrictEqual([sink]);
    const emptied: ServerMessage = { jsonrpc: "2.0", method: "node.companionsChanged", params: { name: "rec", companions: [] } };
    expect(replay([told, emptied], start)!.nodes.find((node) => node.name === "rec")!.companions).toStrictEqual([]);
  });

  it("replaces everything with a snapshot", () => {
    const edited = replay(editsMessages);
    const fresh = fixtures.snapshot.graph;
    const result = fresh as unknown as JsonValue;
    expect(replay([{ jsonrpc: "2.0", id: 9, result }], edited)).toStrictEqual(fresh);
  });

  it("is unchanged by results that are not a graph and by errors", () => {
    const start = replay([snapshotMessage]);
    expect(reduce(start, { jsonrpc: "2.0", id: 2, result: {} })).toBe(start);
    expect(reduce(start, { jsonrpc: "2.0", id: 3, result: { windowsPerSecond: 30 } })).toBe(start);
    expect(reduce(start, { jsonrpc: "2.0", id: 4, error: { code: -32601, message: "Method not found" } })).toBe(
      start,
    );
  });

  it("replaces every health member on node.state: one absent is absent now, one unknown is not kept", () => {
    const start = replay(messagesOf(fixtures.lifecycle.messages).slice(0, 1));
    const waiting: ServerMessage = {
      jsonrpc: "2.0",
      method: "node.state",
      params: {
        name: "desk",
        state: "starting",
        phase: "configuring",
        since: "2026-11-09T10:00:01.000000Z",
        wanted: "running",
        blockLost: false,
        cause: { kind: "action" },
        waitingFor: [{ kind: "companion", name: "desk.playback" }],
      },
    };
    const running: ServerMessage = {
      jsonrpc: "2.0",
      method: "node.state",
      params: {
        name: "desk",
        state: "running",
        phase: "running",
        since: "2026-11-09T10:00:02.000000Z",
        wanted: "running",
        blockLost: false,
        attempt: 3,
      } as unknown as Extract<ServerMessage, { method: "node.state" }>["params"],
    };
    const desk = replay([waiting, running], start)!.nodes.find((node) => node.name === "desk")!;
    expect(desk.phase).toBe("running");
    expect(desk.cause).toBeUndefined();
    expect(desk.waitingFor).toBeUndefined();
    expect("attempt" in desk).toBe(false);
    expect(desk.settings).toStrictEqual({ card: "MOTU", playback: 2 });
  });

  it("keeps a snapshot node's spec and the health members it knows, so an unknown one never goes stale", () => {
    const snapshot = messagesOf(fixtures.lifecycle.messages)[0] as unknown as { jsonrpc: "2.0"; id: number; result: Graph };
    const withExtra = structuredClone(snapshot);
    (withExtra.result.nodes[0] as unknown as Record<string, unknown>).attempt = 3;
    const desk = replay([withExtra as unknown as ServerMessage])!.nodes[0]!;
    expect("attempt" in desk).toBe(false);
    expect(desk).toStrictEqual(snapshot.result.nodes[0]);
  });

  it("takes node.updated's disabled, and drops it when an update leaves it out", () => {
    const start = replay(messagesOf(fixtures.lifecycle.messages).slice(0, 1));
    const disabled = replay(
      [{ jsonrpc: "2.0", method: "node.updated", params: { name: "porch", settings: {}, applied: "disable", disabled: true } }],
      start,
    );
    expect(disabled!.nodes.find((node) => node.name === "porch")!.disabled).toBe(true);
    const enabled = replay(
      [{ jsonrpc: "2.0", method: "node.updated", params: { name: "porch", settings: {}, applied: "enable" } }],
      disabled,
    );
    expect("disabled" in enabled!.nodes.find((node) => node.name === "porch")!).toBe(false);
  });

  it("tells the store's listeners only when the graph changed", () => {
    const store = new GraphStore();
    let told = 0;
    store.subscribe(() => told++);
    store.apply(snapshotMessage);
    store.apply({ jsonrpc: "2.0", method: "node.state", params: { name: "ghost", state: "running" } });
    expect(told).toBe(1);
    expect(store.get()).toStrictEqual(replay([snapshotMessage]));
  });
});

describe("rateLabel", () => {
  it("shows the wire's hertz in kHz, as many decimals as the rate has", () => {
    expect(rateLabel(48000)).toBe("48 kHz");
    expect(rateLabel(44100)).toBe("44.1 kHz");
    expect(rateLabel(11025)).toBe("11.025 kHz");
    expect(rateLabel(768000)).toBe("768 kHz");
  });
});
