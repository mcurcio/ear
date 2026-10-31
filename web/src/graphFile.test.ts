// S6 criterion 1: the graph file reducer turns graph-file.json's messages into its file exactly,
// and the other fixtures (no graphFile.subscribe) leave it null; dropped edits become notices, and a
// failed write stays the file's own state (the pane's one banner for it), never a notice too.
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

/** Where the last batch's graphFile.subscribe answer is (id 16); graphFile.get's answer follows it. */
const resubscribed = messages.findIndex((message) => "id" in message && message.id === 16);

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
    // Everything after the first answer and before the batch's graphFile.subscribe answer.
    expect(replay(messages.slice(1, resubscribed))).toBeNull();
  });

  it("adds a notice for edits a hand edit dropped", () => {
    const state = replay(until((message) => changed(message)?.droppedEdits !== undefined));
    expect(state?.notices).toStrictEqual([{ kind: "dropped", count: 1 }]);
    expect(state?.file).not.toHaveProperty("droppedEdits");
    expect(state?.file).not.toHaveProperty("cause");
  });

  it("keeps a failed write in the file only, adding no notice", () => {
    const failed = until((message) => changed(message)?.writeFailed !== undefined);
    const before = replay(failed.slice(0, -1))!;
    const state = reduceFile(before, failed.at(-1)!)!;
    expect(state.file.writeFailed).toBe(changed(failed.at(-1)!)!.writeFailed);
    expect(state.notices).toStrictEqual(before.notices);
  });

  it("is replaced by a GraphFile result, notices and all", () => {
    expect(resubscribed).toBeGreaterThan(0);
    const state = replay(messages.slice(0, resubscribed))!;
    expect(state.notices.length).toBeGreaterThan(0);
    const fresh = reduceFile(state, messages[resubscribed]!)!;
    expect(fresh).toStrictEqual({ file: fixtures.graphFile.file, notices: [] });
    expect(reduceFile(fresh, messages.at(-1)!)).toStrictEqual(fresh); // graphFile.get's answer: the same file
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
    for (const message of messages.slice(0, resubscribed)) store.apply(message);
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
