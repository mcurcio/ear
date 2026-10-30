// The conformance fixtures (test/protocol, docs/design/batch2.md section 6) cover the whole wire
// format: every method, notification and error code protocol.ts names appears in at least one
// fixture, so the server's and the UI's tests, which both run on these fixtures, exercise all of
// it. Slice W1 owns this file after s0 and adds the reducer's checks.
import { describe, expect, it } from "vitest";
import edits from "../../test/protocol/edits.json";
import errors from "../../test/protocol/errors.json";
import levels from "../../test/protocol/levels.json";
import snapshot from "../../test/protocol/snapshot.json";
import {
  commandErrorCodes,
  methodNames,
  notificationNames,
  rpcErrorCodes,
  type ProtocolFixture,
} from "./protocol";

// JSON imports are typed by their contents (strings widened), so they are read as the wire type.
const fixtures = { snapshot, edits, levels, errors } as unknown as Record<string, ProtocolFixture>;
const all = Object.values(fixtures);

/** The method of every request a fixture sends that parses as JSON-RPC. */
function requestMethods(): Set<string> {
  const methods = new Set<string>();
  for (const fixture of all) {
    for (const text of fixture.requests) {
      try {
        const request: unknown = JSON.parse(text);
        if (typeof request === "object" && request !== null && "method" in request) {
          methods.add(String(request.method));
        }
      } catch {
        // an unparseable request (the parse error case) names no method
      }
    }
  }
  return methods;
}

function notificationMethods(): Set<string> {
  const methods = new Set<string>();
  for (const fixture of all) {
    for (const message of fixture.messages) {
      if ("method" in message && !("id" in message)) methods.add(message.method);
    }
  }
  return methods;
}

function errorCodesSeen(): { rpc: Set<number>; command: Set<string> } {
  const rpc = new Set<number>();
  const command = new Set<string>();
  for (const fixture of all) {
    for (const message of fixture.messages) {
      if (!("error" in message)) continue;
      rpc.add(message.error.code);
      if (message.error.data !== undefined) command.add(message.error.data.code);
    }
  }
  return { rpc, command };
}

describe("the protocol fixtures", () => {
  it("each has a description, requests, messages and the graph after them", () => {
    for (const [name, fixture] of Object.entries(fixtures)) {
      expect(typeof fixture.description, name).toBe("string");
      expect(Array.isArray(fixture.requests), name).toBe(true);
      expect(fixture.messages.length, name).toBeGreaterThan(0);
      expect(Array.isArray(fixture.graph.nodes), name).toBe(true);
      for (const message of fixture.messages) expect(message.jsonrpc, name).toBe("2.0");
    }
  });

  it("send every method", () => {
    const sent = requestMethods();
    for (const method of methodNames) expect(sent, method).toContain(method);
  });

  it("receive every notification", () => {
    const received = notificationMethods();
    for (const method of notificationNames) expect(received, method).toContain(method);
  });

  it("receive every command error code and every JSON-RPC error code", () => {
    const { rpc, command } = errorCodesSeen();
    for (const code of commandErrorCodes) expect(command, code).toContain(code);
    for (const code of Object.values(rpcErrorCodes)) expect(rpc, String(code)).toContain(code);
  });
});
