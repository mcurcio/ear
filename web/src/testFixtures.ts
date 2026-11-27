// The conformance fixtures (test/protocol, docs/control-port.md; docs/control-port.md#fixtures
// from protocol 1), read as the wire type, for the UI's tests only. The server's tests (ear_control)
// run on the same files. FakeSocket is the client's socket for the rpc and editor tests.
import edits from "../../test/protocol/edits.json";
import companions from "../../test/protocol/companions.json";
import countersChanged from "../../test/protocol/counters-changed.json";
import errors from "../../test/protocol/errors.json";
import graphFile from "../../test/protocol/graph-file.json";
import levels from "../../test/protocol/levels.json";
import lifecycle from "../../test/protocol/lifecycle.json";
import nodeSet from "../../test/protocol/node-set.json";
import nodeText from "../../test/protocol/node-text.json";
import snapshot from "../../test/protocol/snapshot.json";
import types from "../../test/protocol/types.json";
import unknownValues from "../../test/protocol/unknown-values.json";
import status from "../../test/protocol/status.json";
import diagnostics from "../../test/protocol/diagnostics.json";
import actions from "../../test/protocol/actions.json";
import gestures from "../../test/protocol/gestures.json";
import type { ProtocolFixture, ServerMessage, ServerText } from "./protocol";
import type { SocketEvents, SocketLike } from "./rpc";

// JSON imports are typed by their contents (strings widened), so they are read as the wire type.
export const fixtures = {
  snapshot,
  edits,
  levels,
  errors,
  graphFile,
  lifecycle,
  unknownValues,
  countersChanged,
  companions,
  nodeText,
  nodeSet,
  types,
  status,
  diagnostics,
  actions,
  gestures,
} as unknown as {
  snapshot: ProtocolFixture;
  edits: ProtocolFixture;
  levels: ProtocolFixture;
  errors: ProtocolFixture;
  graphFile: ProtocolFixture;
  lifecycle: ProtocolFixture;
  unknownValues: ProtocolFixture;
  countersChanged: ProtocolFixture;
  companions: ProtocolFixture;
  nodeText: ProtocolFixture;
  nodeSet: ProtocolFixture;
  types: ProtocolFixture;
  status: ProtocolFixture;
  diagnostics: ProtocolFixture;
  actions: ProtocolFixture;
  gestures: ProtocolFixture;
};

/** A fixture's server texts as the messages the client hands on, a batch's in its order. */
export function messagesOf(texts: ServerText[]): ServerMessage[] {
  return texts.flatMap((text) => (Array.isArray(text) ? text : [text]));
}

/** A socket the test drives: what the client sent, and the server's side of open, message and close. */
export class FakeSocket implements SocketLike {
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
