// S6 criterion 1: the graph file reducer turns graph-file.json's messages into its file exactly,
// and the other fixtures (no graphFile.subscribe) leave it null; dropped edits and a newly failed
// write become notices.
import { describe, expect, it } from "vitest";
import { GraphFileStore, dismissNotice, reduceFile, type GraphFileState } from "./graphFile";
import type { Notifications, ServerMessage } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";

function replay(messages: ServerMessage[], start: GraphFileState | null = null): GraphFileState | null {
  return messages.reduce<GraphFileState | null>(reduceFile, start);
}

const messages = messagesOf(fixtures.graphFile.messages);

/** The messages up to and including the first that matches. */
function until(match: (message: ServerMessage) => boolean): ServerMessage[] {
  const at = messages.findIndex(match);
  expect(at).toBeGreaterThanOrEqual(0);
  return messages.slice(0, at + 1);
}

const changed = (message: ServerMessage): Notifications["graphFile.changed"] | null =>
  "method" in message && message.method === "graphFile.changed"
    ? (message.params as Notifications["graphFile.changed"])
    : null;

describe("the graph file reducer", () => {
  it("turns the graph-file fixture's messages into its file", () => {
    expect(fixtures.graphFile.file).toBeDefined();
    expect(replay(messages)?.file).toStrictEqual(fixtures.graphFile.file);
  });

  for (const name of ["snapshot", "edits", "levels", "errors"] as const) {
    it(`leaves the ${name} fixture's state null`, () => {
      expect(replay(messagesOf(fixtures[name].messages))).toBeNull();
    });
  }

  it("ignores a change before the first file", () => {
    // Everything but the first answer and the last (the batch's graphFile.subscribe answer).
    expect(replay(messages.slice(1, -1))).toBeNull();
  });

  it("adds a notice for edits a hand edit dropped", () => {
    const state = replay(until((message) => changed(message)?.droppedEdits !== undefined));
    expect(state?.notices).toStrictEqual([{ kind: "dropped", count: 1 }]);
    expect(state?.file).not.toHaveProperty("droppedEdits");
    expect(state?.file).not.toHaveProperty("cause");
  });

  it("adds a notice for a newly failed write, once", () => {
    const failed = until((message) => changed(message)?.writeFailed !== undefined);
    const state = replay(failed)!;
    const detail = state.file.writeFailed!;
    expect(state.notices.at(-1)).toStrictEqual({ kind: "writeFailed", detail });
    const again = reduceFile(state, failed.at(-1)!)!;
    expect(again.notices).toStrictEqual(state.notices);
  });

  it("is replaced by a GraphFile result, notices and all", () => {
    const state = replay(messages.slice(0, -1))!;
    expect(state.notices.length).toBeGreaterThan(0);
    const fresh = reduceFile(state, messages.at(-1)!)!;
    expect(fresh).toStrictEqual({ file: fixtures.graphFile.file, notices: [] });
  });

  it("is unchanged by graph results, errors and graph events", () => {
    const state = replay(messages.slice(0, 1));
    for (const message of messages) {
      if (changed(message) !== null) continue;
      if ("result" in message && typeof message.result === "object" && message.result !== null && "version" in message.result) {
        continue;
      }
      expect(reduceFile(state, message)).toBe(state);
    }
  });

  it("dismisses one notice; the store tells its listeners only on a change", () => {
    const store = new GraphFileStore();
    let told = 0;
    store.subscribe(() => told++);
    for (const message of messages.slice(0, -1)) store.apply(message);
    const before = told;
    store.apply({ jsonrpc: "2.0", method: "node.state", params: { name: "ghost", state: "running" } });
    expect(told).toBe(before);
    const notices = store.get()!.notices;
    const first = notices[0]!;
    store.dismiss(first);
    expect(told).toBe(before + 1);
    expect(store.get()!.notices).toStrictEqual(notices.slice(1));
    expect(dismissNotice(store.get()!, first)).toStrictEqual(store.get());
  });

  it("stays null when graphFile.subscribe is refused", () => {
    const refused: ServerMessage = { jsonrpc: "2.0", id: 1, error: { code: -32601, message: "Method not found" } };
    expect(reduceFile(null, refused)).toBeNull();
  });
});
