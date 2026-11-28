// The session's toasts (docs/editor.md#toasts) on patchbay's queue and a fake monotonic clock, keyed by code and
// subject, and a refusal's words, replaying errors.json's tracked requests with their answers.
import { toastDefaults } from "@mcurcio/patchbay/hooks";
import { describe, expect, it } from "vitest";
import { edgeId } from "./connect";
import {
  noticeOfRefusal,
  noticeOfState,
  Notices,
  placedRefusal,
  refusalParts,
  showOf,
  trimCode,
  type NoticeInput,
} from "./notices";
import type { Notifications, RouteEnds, RpcErrorObject } from "./protocol";
import { askedOf, type AskedMethod } from "./requests";
import { ConnectionClosedError, RpcError } from "./rpc";
import { fixtures, messagesOf } from "./testFixtures";

const input = (code: string, node?: string): NoticeInput => ({
  severity: "error",
  code,
  what: node === undefined ? "save ear.conf" : `Start ${node}`,
  verb: "refused",
  detail: "",
  subject: node === undefined ? undefined : { kind: "node", node },
});

/** errors.json's requests by id, each with ear's answer. */
function errorsById(): Map<number, { method: string; params: unknown; error: RpcErrorObject }> {
  const answers = new Map<number, RpcErrorObject>();
  for (const message of messagesOf(fixtures.errors.messages)) {
    if ("error" in message && typeof message.id === "number") answers.set(message.id, message.error);
  }
  const byId = new Map<number, { method: string; params: unknown; error: RpcErrorObject }>();
  for (const text of fixtures.errors.requests) {
    let request: { id?: unknown; method?: unknown; params?: unknown };
    try {
      request = JSON.parse(text);
    } catch {
      continue;
    }
    const error = typeof request.id === "number" ? answers.get(request.id) : undefined;
    if (error !== undefined && typeof request.method === "string")
      byId.set(request.id as number, { method: request.method, params: request.params, error });
  }
  return byId;
}

function refusalOf(id: number): NoticeInput {
  const request = errorsById().get(id)!;
  const asked = askedOf(request.method as AskedMethod, request.params as never);
  return noticeOfRefusal(asked, new RpcError(request.error));
}

const noticeMs = toastDefaults.duration;
const coalesceMs = toastDefaults.coalesceWindow;

describe("the toast queue", () => {
  it("keeps a notice for 8 s on the monotonic clock", () => {
    expect(noticeMs).toBe(8000);
    let clock = 1000;
    const notices = new Notices(() => clock);
    notices.add(input("node-disabled", "porch"));
    const [notice] = notices.get().notices;
    expect(notice).toMatchObject({ id: 1, count: 1, lastAt: 1000, expiresAt: 1000 + noticeMs });
    expect(notices.nextExpiry()).toBe(1000 + noticeMs);
    clock = 1000 + noticeMs - 1;
    notices.expire();
    expect(notices.get().notices).toHaveLength(1);
    clock = 1000 + noticeMs;
    notices.expire();
    expect(notices.get().notices).toEqual([]);
    expect(notices.nextExpiry()).toBeUndefined();
  });

  it("counts up the same code for the same subject within 5 s, moved to the newest", () => {
    expect(coalesceMs).toBe(5000);
    let clock = 0;
    const notices = new Notices(() => clock);
    notices.add(input("node-disabled", "porch"));
    clock = 100;
    notices.add(input("unknown-node", "ghost"));
    clock = 2000;
    notices.add(input("node-disabled", "porch"));
    clock = 4000;
    notices.add(input("node-disabled", "porch"));
    const view = notices.get().notices;
    expect(view).toHaveLength(2);
    expect(view[0]!.code).toBe("unknown-node");
    expect(view[1]).toMatchObject({ id: 1, code: "node-disabled", count: 3, lastAt: 4000, expiresAt: 4000 + noticeMs });

    // Another subject, another code, or after 5 s: a new notice.
    notices.add(input("node-disabled", "desk"));
    notices.add(input("not-restartable", "porch"));
    clock = 4000 + coalesceMs;
    notices.add(input("node-disabled", "porch"));
    expect(notices.get().notices.map((notice) => [notice.code, notice.count])).toEqual([
      ["unknown-node", 1],
      ["node-disabled", 3],
      ["node-disabled", 1],
      ["not-restartable", 1],
      ["node-disabled", 1],
    ]);
  });

  it("shows the newest four, oldest first, and folds the rest into a count", () => {
    expect(toastDefaults.visible).toBe(4);
    let clock = 0;
    const notices = new Notices(() => clock);
    for (const code of ["a", "b", "c", "d", "e", "f"]) {
      clock += 10;
      notices.add(input(code));
    }
    let { shown, more } = notices.get();
    expect(shown.map((notice) => notice.code)).toEqual(["c", "d", "e", "f"]);
    expect(more).toBe(2);
    notices.dismiss(shown[1]!.id);
    ({ shown, more } = notices.get());
    expect(shown.map((notice) => notice.code)).toEqual(["b", "c", "e", "f"]);
    expect(more).toBe(1);
  });

  it("expires nothing while held, and moves each expiry by the time held", () => {
    let clock = 0;
    const notices = new Notices(() => clock);
    notices.add(input("stopping", "noise"));
    clock = 1000;
    notices.hold(true);
    expect(notices.nextExpiry()).toBeUndefined();
    clock = 4000;
    notices.hold(true); // held already: since stays 1000
    clock = noticeMs + 500;
    notices.expire();
    expect(notices.get().notices).toHaveLength(1);
    clock = noticeMs + 2000;
    notices.hold(false);
    expect(notices.nextExpiry()).toBe(noticeMs + (noticeMs + 2000 - 1000));
    expect(notices.get().heldSince).toBeUndefined();
  });

  it("draws a refusal as an err toast and a warning as a warn one, in its words, keyed by code and subject", () => {
    const notices = new Notices(() => 0);
    notices.add({ ...input("unknown-port", "rec"), detail: "rec:FX" });
    notices.add({ ...input("not-connected"), severity: "warning", verb: "had no answer" });
    expect(notices.get().notices.map(({ tone, text, key }) => ({ tone, text, key }))).toStrictEqual([
      { tone: "err", text: "Start rec refused: unknown-port rec:FX", key: "unknown-port\nnode rec" },
      { tone: "warn", text: "save ear.conf had no answer: not-connected", key: "not-connected\n" },
    ]);
  });

  it("is the same object between changes, and tells its listeners on each", () => {
    const notices = new Notices(() => 0);
    let told = 0;
    notices.subscribe(() => told++);
    const empty = notices.get();
    notices.dismiss(99);
    notices.hold(false);
    notices.expire();
    expect(notices.get()).toBe(empty);
    expect(told).toBe(0);
    notices.add(input("x"));
    expect(notices.get()).not.toBe(empty);
    expect(told).toBe(1);
  });
});

describe("a refusal in words", () => {
  it("names the route ear refused, its code and the rest of ear's message", () => {
    const ends: RouteEnds = { from: { node: "noise", port: "noise.mono" }, to: { node: "rec", port: "FX" } };
    expect(refusalOf(8)).toStrictEqual({
      severity: "error",
      what: "route noise:noise.mono → rec:FX",
      verb: "refused",
      code: "unknown-port",
      detail: "rec:FX",
      subject: { kind: "route", ends },
    });
  });

  it("names the node of a lifecycle refusal", () => {
    expect(refusalOf(14)).toMatchObject({
      what: "Start porch",
      code: "node-disabled",
      detail: "porch",
      subject: { kind: "node", node: "porch" },
    });
    expect(refusalOf(15)).toMatchObject({ what: "Restart desk", code: "not-restartable", detail: "desk: block-lost" });
  });

  it("words invalid params by their code and says the pointer once", () => {
    expect(refusalOf(13)).toMatchObject({ code: "invalid-params", detail: "/from/port: missing" });
    expect(refusalParts(new RpcError({ code: -32602, message: "missing", data: { pointer: "/name" } }))).toMatchObject({
      code: "invalid-params",
      detail: "missing at /name",
    });
    expect(refusalParts(new RpcError({ code: -32601, message: "Method not found" })).code).toBe("method-not-found");
    expect(refusalParts(new RpcError({ code: -32099 as never, message: "odd" })).code).toBe("-32099");
  });

  it("gives an empty detail for a message that is only its code", () => {
    expect(refusalOf(11)).toMatchObject({ what: "add n3", code: "stopping", detail: "" });
    expect(trimCode("stopping", "stopping")).toBe("");
    expect(trimCode("unknown-node ghost", "unknown-node")).toBe("ghost");
    expect(trimCode("Parse error", "parse-error")).toBe("Parse error");
  });

  it("words every errors.json refusal of a tracked request with a code and a subject", () => {
    const named = ["route.add", "route.remove", "node.add", "node.remove", "node.update", "node.start", "node.stop"];
    let count = 0;
    for (const [, request] of errorsById()) {
      if (![...named, "node.restart"].includes(request.method)) continue;
      const asked = askedOf(request.method as AskedMethod, request.params as never);
      const notice = noticeOfRefusal(asked, new RpcError(request.error));
      expect(notice.verb).toBe("refused");
      expect(notice.code).not.toBe("");
      expect(notice.subject).toBeDefined();
      count++;
    }
    expect(count).toBeGreaterThan(15);
  });

  it("folds every lost answer into one warning with no subject", () => {
    const parts = refusalParts(new ConnectionClosedError());
    expect(parts).toStrictEqual({
      verb: "had no answer",
      code: "not-connected",
      detail: "the connection to ear closed",
      severity: "warning",
    });
    const notices = new Notices(() => 0);
    notices.add(noticeOfRefusal({ method: "node.start", node: "porch" }, new ConnectionClosedError()));
    notices.add(noticeOfRefusal({ method: "graphFile.set" }, new ConnectionClosedError()));
    const view = notices.get().notices;
    expect(view).toHaveLength(1);
    expect(view[0]).toMatchObject({ code: "not-connected", count: 2, subject: undefined });
  });

  it("says a settings text that does not parse was not sent", () => {
    const error = new Error("settings are not JSON");
    expect(noticeOfRefusal({ method: "node.update", node: "porch" }, error)).toMatchObject({
      what: "porch's settings",
      verb: "not sent",
      code: "invalid-json",
      detail: "settings are not JSON",
    });
  });

  it("leaves file-changed and invalid-text to the form that asked", () => {
    const refused = (code: "file-changed" | "invalid-text" | "unknown-node") =>
      new RpcError({ code: -32000, message: code, data: { code } });
    expect(placedRefusal(refused("file-changed"))).toBe(true);
    expect(placedRefusal(refused("invalid-text"))).toBe(true);
    expect(placedRefusal(refused("unknown-node"))).toBe(false);
    expect(placedRefusal(new RpcError({ code: -32602, message: "invalid-text" }))).toBe(false);
    expect(placedRefusal(new ConnectionClosedError())).toBe(false);
    expect(placedRefusal(new Error("file-changed"))).toBe(false);
  });
});

describe("a toast's Show", () => {
  const graph = fixtures.snapshot.graph;

  it("selects a node ear holds and pans to it", () => {
    expect(showOf(graph, { kind: "node", node: "rec" })).toStrictEqual({
      selection: { nodes: ["rec"], routes: [] },
      nodes: ["rec"],
    });
  });

  it("selects a route ear holds and pans to both its nodes", () => {
    const held = graph.routes[1]!;
    const ends = { from: held.from, to: held.to };
    expect(showOf(graph, { kind: "route", ends })).toStrictEqual({
      selection: { nodes: [], routes: [edgeId(held)] },
      nodes: [held.from.node, held.to.node],
    });
  });

  it("offers nothing for what ear does not hold", () => {
    expect(showOf(graph, { kind: "node", node: "ghost" })).toBeNull();
    const ends: RouteEnds = { from: { node: "noise", port: "noise.mono" }, to: { node: "rec", port: "FX" } };
    expect(showOf(graph, { kind: "route", ends })).toBeNull();
    expect(showOf(graph, undefined)).toBeNull();
    expect(showOf(null, { kind: "node", node: "rec" })).toBeNull();
  });
});

describe("a change in place that fell back", () => {
  it("raises one warning toast from lifecycle.json's fallback, about its node, and none for any other state", () => {
    const raised = messagesOf(fixtures.lifecycle.messages).flatMap((message) =>
      "method" in message && message.method === "node.state"
        ? [noticeOfState(message.params as Notifications["node.state"])].filter((notice) => notice !== undefined)
        : [],
    );
    expect(raised).toStrictEqual([
      {
        severity: "warning",
        code: "fell-back",
        what: "fpp",
        verb: "could not apply format in place, so it restarted",
        detail: "deadline 10s passed",
        subject: { kind: "node", node: "fpp" },
      },
    ]);
  });
});
