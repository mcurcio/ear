// A node's status list, box fields, callouts and badges, and its activity and toasts (activity.ts,
// docs/editor.md#status-events-and-diagnostics), against the status and diagnostics fixtures.
import { describe, expect, it } from "vitest";
import {
  Activity,
  activityCap,
  boxStatus,
  diagnosticBadges,
  noticePerNodeMs,
  statusRows,
  valueWords,
} from "./activity";
import { reduce, type GraphState } from "./graph";
import type { GraphNode, ServerMessage, TypeSchema } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";

const schema: TypeSchema = {
  name: "sendspin",
  settings: [],
  inputs: [],
  outputs: [],
  reconfigureMs: 10000,
  status: [
    { name: "connection", kind: "choice", initial: null, display: "box", flapsAbove: { transitions: 10, windowMs: 10000 }, values: ["dialling", "connected"] },
    { name: "paired", kind: "flag", initial: false, display: "panel", flapsAbove: { transitions: 10, windowMs: 10000 } },
    { name: "pairingCode", kind: "text", initial: "", display: "callout", flapsAbove: { transitions: 10, windowMs: 10000 }, maxBytes: 16, help: "type it into Music Assistant" },
    { name: "streamRate", kind: "wholeNumber", initial: 0, display: "box", flapsAbove: { transitions: 10, windowMs: 10000 } },
  ],
  events: [],
};

function kitchenAfter(messages: ServerMessage[]): GraphNode {
  const graph = messages.reduce<GraphState>(reduce, null);
  const node = graph?.nodes.find((one) => one.name === "kitchen");
  if (node === undefined) throw new Error("no kitchen");
  return node;
}

describe("a node's status in the editor", () => {
  const messages = messagesOf(fixtures.status.messages);

  it("lists every field in words, in the schema's order, null as unknown and a flag as yes or no", () => {
    const node = kitchenAfter(messages.slice(0, 1));
    expect(statusRows(node, schema)).toStrictEqual([
      { name: "connection", text: "unknown" },
      { name: "paired", text: "no" },
      { name: "pairingCode", text: "", help: "type it into Music Assistant" },
      { name: "streamRate", text: "0" },
    ]);
    expect(valueWords(true)).toBe("yes");
  });

  it("draws box fields and a callout not at its initial value, only while running, and never a null", () => {
    expect(boxStatus(kitchenAfter(messages.slice(0, 1)), schema)).toStrictEqual({ line: "0", callouts: [] });
    const pairing = kitchenAfter(messages.slice(0, 3));
    expect(boxStatus(pairing, schema)).toStrictEqual({
      line: "connected · 0",
      callouts: [{ name: "pairingCode", text: "123-456" }],
    });
    expect(boxStatus({ ...pairing, state: "failed" }, schema)).toStrictEqual({ line: "", callouts: [] });
    expect(boxStatus(pairing)).toStrictEqual({ line: "", callouts: [] }); // no schema held: none
  });
});

describe("a node's diagnostics in the editor", () => {
  it("badges each active entry, never an observation, an unknown code by its own words", () => {
    const node = kitchenAfter(messagesOf(fixtures.diagnostics.messages));
    const badges = diagnosticBadges(node);
    expect(badges.map((badge) => badge.label)).toStrictEqual(["flapping: streamRate", "cosmic ray: left"]);
    expect(badges[0]!.title).toBe("streamRate flapping: 11 changes in 10s (2 times; 11 changes in 10 s)");
  });
});

describe("a node's activity", () => {
  it("keeps each event and diagnostic of its node, the newest last, at most activityCap", () => {
    const activity = new Activity(() => 0);
    for (const message of messagesOf(fixtures.status.messages)) activity.apply(message);
    for (const message of messagesOf(fixtures.diagnostics.messages)) activity.apply(message);
    expect(activity.of("kitchen").map((entry) => entry.code)).toStrictEqual([
      "pairing-failed",
      "ear.status-flapping",
      "ear.status-flapping",
      "ear.events-dropped",
      "ear.status-flapping",
      "ear.cosmic-ray",
    ]);
    expect(activity.of("ghost").length).toBe(2);
    for (let i = 0; i < activityCap + 5; ++i)
      activity.apply({
        jsonrpc: "2.0",
        method: "node.eventRaised",
        params: { name: "kitchen", code: "tick", severity: "info", t: "", args: {}, msg: `tick ${i}` },
      });
    expect(activity.of("kitchen").length).toBe(activityCap);
    expect(activity.of("kitchen").at(-1)!.msg).toBe(`tick ${activityCap + 4}`);
  });

  it("toasts a warn or error event, at most one new toast per node per 5 s", () => {
    let now = 0;
    const activity = new Activity(() => now);
    const event = (severity: string, code: string): ServerMessage => ({
      jsonrpc: "2.0",
      method: "node.eventRaised",
      params: { name: "kitchen", code, severity, t: "", args: {}, msg: `${code} happened` },
    });
    expect(activity.apply(event("info", "paired"))).toBeUndefined();
    expect(activity.apply(event("warn", "pairing-failed"))).toStrictEqual({
      severity: "warning",
      code: "pairing-failed",
      what: "kitchen",
      verb: "says",
      detail: "pairing-failed happened",
      subject: { kind: "node", node: "kitchen" },
    });
    now = noticePerNodeMs - 1;
    expect(activity.apply(event("error", "key-write-failed"))).toBeUndefined();
    now = noticePerNodeMs;
    expect(activity.apply(event("error", "key-write-failed"))?.severity).toBe("error");
  });
});
