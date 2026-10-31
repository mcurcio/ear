// W1 criterion (1): the reducer turns every fixture's messages into its graph exactly; events
// naming unknown nodes are ignored; a snapshot replaces the state.
import { describe, expect, it } from "vitest";
import { GraphStore, reduce, type GraphState } from "./graph";
import type { JsonValue, ServerMessage } from "./protocol";
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
        params: { from: { node: "ghost", port: "MONO" }, to: { node: "rec", port: "FR" } },
      },
      {
        jsonrpc: "2.0",
        method: "route.status",
        params: { from: { node: "noise", port: "MONO" }, to: { node: "rec", port: "FR" }, status: "linked" },
      },
      {
        jsonrpc: "2.0",
        method: "route.removed",
        params: { from: { node: "noise", port: "MONO" }, to: { node: "rec", port: "FR" } },
      },
    ];
    expect(replay(ghostly, start)).toBe(start);
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
