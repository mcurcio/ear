// The editor session's connection view (docs/editor.md#the-top-bar), on fake sockets and a fake clock: the status, and
// while closed when the next socket opens, the same object between changes.
import { describe, expect, it } from "vitest";
import { Editor } from "./editor";
import { firstRetryMs, type SocketEvents } from "./rpc";
import { FakeSocket } from "./testFixtures";

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
