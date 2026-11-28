// The requests in flight and their ghosts (docs/editor.md#requests-in-flight), on a fake monotonic clock, replaying
// the errors and lifecycle fixtures' requests.
import { describe, expect, it } from "vitest";
import { edgeId } from "./connect";
import type { Methods, RouteEnds } from "./protocol";
import {
  askedOf,
  describeAsked,
  ghostRefusedMs,
  ghostNode,
  ghostWires,
  isAsking,
  lifecycleAsked,
  Requests,
  type Asked,
  type AskedMethod,
} from "./requests";
import { fixtures } from "./testFixtures";

const tracked = new Set<string>([
  "route.add",
  "route.remove",
  "node.add",
  "node.addText",
  "node.remove",
  "node.update",
  "node.setText",
  "node.start",
  "node.stop",
  "node.restart",
  "node.disable",
  "node.enable",
  "graphFile.set",
]);

/** A fixture's requests that are tracked methods, as sent (a batch's in its order; unparseable ones skipped). */
function trackedRequests(texts: string[]): { method: AskedMethod; params: Methods[AskedMethod]["params"] }[] {
  const requests: { method: AskedMethod; params: Methods[AskedMethod]["params"] }[] = [];
  for (const text of texts) {
    let parsed: unknown;
    try {
      parsed = JSON.parse(text);
    } catch {
      continue;
    }
    for (const request of Array.isArray(parsed) ? parsed : [parsed]) {
      if (typeof request.method === "string" && tracked.has(request.method)) requests.push(request);
    }
  }
  return requests;
}

const route = (from: string, to: string): RouteEnds => {
  const [fromNode, fromPort] = from.split(":") as [string, string];
  const [toNode, toPort] = to.split(":") as [string, string];
  return { from: { node: fromNode, port: fromPort }, to: { node: toNode, port: toPort } };
};

describe("what asked for a request", () => {
  it("keeps a route's ends, a node's name, or nothing, for every tracked request the fixtures send", () => {
    const requests = [...trackedRequests(fixtures.errors.requests), ...trackedRequests(fixtures.lifecycle.requests)];
    expect(requests.length).toBeGreaterThan(20);
    for (const { method, params } of requests) {
      const asked = askedOf(method, params);
      expect(asked.method).toBe(method);
      if (method === "route.add" || method === "route.remove") {
        const { from, to } = params as RouteEnds;
        expect(asked).toStrictEqual({ method, ends: { from, to } });
      } else {
        expect(asked).toStrictEqual({ method, node: (params as { name: string }).name });
      }
    }
  });

  it("keeps only from and to of a route, and no subject for node.addText or graphFile.set", () => {
    const extra = { ...route("noise:noise.mono", "rec:FX"), colour: "red" } as unknown as RouteEnds;
    const ends = route("noise:noise.mono", "rec:FX");
    expect(askedOf("route.remove", extra)).toStrictEqual({ method: "route.remove", ends });
    const addText = { text: "node white w {\n}", version: "v1" };
    expect(askedOf("node.addText", addText)).toStrictEqual({ method: "node.addText" });
    expect(askedOf("graphFile.set", { text: "", version: "v1" })).toStrictEqual({ method: "graphFile.set" });
    expect(askedOf("node.setText", { name: "rec", text: "", version: "v1" })).toStrictEqual({
      method: "node.setText",
      node: "rec",
    });
  });

  it("says what was asked in words", () => {
    const ends = route("noise:noise.mono", "rec:FX");
    const words: [Asked, string][] = [
      [{ method: "route.add", ends }, "route noise:noise.mono → rec:FX"],
      [{ method: "route.remove", ends }, "remove route noise:noise.mono → rec:FX"],
      [{ method: "node.add", node: "tape" }, "add tape"],
      [{ method: "node.addText" }, "add node"],
      [{ method: "node.remove", node: "ghost" }, "remove ghost"],
      [{ method: "node.update", node: "porch" }, "porch's settings"],
      [{ method: "node.setText", node: "rec" }, "save rec's text"],
      [{ method: "node.start", node: "porch" }, "Start porch"],
      [{ method: "node.stop", node: "tape" }, "Stop tape"],
      [{ method: "node.restart", node: "desk" }, "Restart desk"],
      [{ method: "node.disable", node: "porch" }, "Disable porch"],
      [{ method: "node.enable", node: "porch" }, "Enable porch"],
      [{ method: "graphFile.set" }, "save ear.conf"],
    ];
    for (const [asked, text] of words) expect(describeAsked(asked)).toBe(text);
  });
});

describe("the requests in flight", () => {
  it("holds a request from begin until its answer, one view object between changes", () => {
    const requests = new Requests(() => 100);
    let told = 0;
    requests.subscribe(() => told++);
    const empty = requests.get();
    expect(empty.inFlight).toEqual([]);

    const token = requests.begin({ method: "node.start", node: "porch" });
    const asking = requests.get();
    expect(asking).not.toBe(empty);
    expect(asking.inFlight).toStrictEqual([{ token, asked: { method: "node.start", node: "porch" }, since: 100 }]);
    expect(requests.get()).toBe(asking);
    expect(told).toBe(1);

    requests.end(token, "answered");
    expect(requests.get().inFlight).toEqual([]);
    expect(requests.get().refused).toEqual([]);
    expect(told).toBe(2);

    // No-ops tell no one and keep the view.
    const after = requests.get();
    requests.end(token, "answered");
    requests.expire();
    requests.clear();
    expect(requests.get()).toBe(after);
    expect(told).toBe(2);
  });

  it("forgets everything when the connection closes, and a cleared token's end changes nothing", () => {
    let clock = 0;
    const requests = new Requests(() => clock);
    const refusedToken = requests.begin({ method: "route.add", ends: route("a:o", "b:i") });
    requests.end(refusedToken, "refused");
    const token = requests.begin({ method: "route.add", ends: route("noise:noise.mono", "rec:FX") });
    requests.begin({ method: "graphFile.set" });
    expect(requests.get().inFlight).toHaveLength(2);
    expect(requests.get().refused).toHaveLength(1);

    requests.clear();
    expect(requests.get().inFlight).toEqual([]);
    expect(requests.get().refused).toEqual([]);

    let told = 0;
    requests.subscribe(() => told++);
    clock = 10;
    requests.end(token, "refused");
    expect(requests.get().refused).toEqual([]);
    expect(told).toBe(0);
  });

  it("tells which lifecycle methods are in flight for one node, and whether a method is asked", () => {
    const requests = new Requests(() => 0);
    requests.begin({ method: "node.start", node: "porch" });
    requests.begin({ method: "node.restart", node: "porch" });
    requests.begin({ method: "node.stop", node: "desk" });
    requests.begin({ method: "node.setText", node: "rec" });
    requests.begin({ method: "node.addText" });
    const view = requests.get();
    expect([...lifecycleAsked(view, "porch")].sort()).toEqual(["node.restart", "node.start"]);
    expect([...lifecycleAsked(view, "desk")]).toEqual(["node.stop"]);
    expect([...lifecycleAsked(view, "rec")]).toEqual([]);
    expect(isAsking(view, "node.setText", "rec")).toBe(true);
    expect(isAsking(view, "node.setText", "tape")).toBe(false);
    expect(isAsking(view, "node.setText")).toBe(true);
    expect(isAsking(view, "node.addText")).toBe(true);
    expect(isAsking(view, "graphFile.set")).toBe(false);
    expect(isAsking(view, "node.start", "desk")).toBe(false);
  });
});

describe("the ghost wires", () => {
  it("draws one per route.add in flight, and none for any other request", () => {
    const requests = new Requests(() => 0);
    const ends = route("noise:noise.mono", "rec:FX");
    const token = requests.begin({ method: "route.add", ends });
    requests.begin({ method: "route.remove", ends: route("noise:noise.mono", "rec:record.left") });
    requests.begin({ method: "node.start", node: "noise" });
    requests.begin({ method: "graphFile.set" });
    const second = requests.begin({ method: "route.add", ends: route("noise:noise.mono", "rec:record.right") });
    expect(ghostWires(requests.get(), [])).toStrictEqual([
      { id: `ghost:${token}`, ends, refused: false },
      { id: `ghost:${second}`, ends: route("noise:noise.mono", "rec:record.right"), refused: false },
    ]);
  });

  it("hides a ghost once the graph holds its route", () => {
    const requests = new Requests(() => 0);
    const graph = fixtures.snapshot.graph;
    const held = graph.routes[1]!;
    requests.begin({ method: "route.add", ends: { from: held.from, to: held.to } });
    expect(ghostWires(requests.get(), graph.routes)).toEqual([]);
    expect(ghostWires(requests.get(), []).map((ghost) => edgeId(ghost.ends))).toEqual([edgeId(held)]);
  });

  it("keeps a refused route request's ghost, refused, for a second on the monotonic clock", () => {
    let clock = 1000;
    const requests = new Requests(() => clock);
    const ends = route("noise:noise.mono", "rec:FX");
    const token = requests.begin({ method: "route.add", ends });
    expect(requests.nextChange()).toBeUndefined();
    clock = 1200;
    requests.end(token, "refused");
    expect(ghostWires(requests.get(), [])).toStrictEqual([{ id: `ghost:${token}`, ends, refused: true }]);
    expect(requests.nextChange()).toBe(1200 + ghostRefusedMs);

    clock = 1200 + ghostRefusedMs - 1;
    requests.expire();
    expect(ghostWires(requests.get(), [])).toHaveLength(1);
    clock = 1200 + ghostRefusedMs;
    requests.expire();
    expect(ghostWires(requests.get(), [])).toEqual([]);
    expect(requests.nextChange()).toBeUndefined();
  });

  it("leaves no ghost for a route request answered or lost, nor for any other method refused", () => {
    const requests = new Requests(() => 0);
    const lost = requests.begin({ method: "route.add", ends: route("a:o", "b:i") });
    const answered = requests.begin({ method: "route.add", ends: route("a:o", "c:i") });
    const removal = requests.begin({ method: "route.remove", ends: route("a:o", "d:i") });
    requests.end(lost, "lost");
    requests.end(answered, "answered");
    requests.end(removal, "refused");
    expect(ghostWires(requests.get(), [])).toEqual([]);
    expect(requests.get().refused).toEqual([]);
  });
});

describe("the ghost node", () => {
  const place = { x: 10, y: 20 };

  it("stands where quick add holds a place, only while an add is on its way", () => {
    const requests = new Requests(() => 0);
    expect(ghostNode(requests.get(), { name: "white1", position: place })).toBeNull();
    const token = requests.begin({ method: "node.addText" });
    expect(ghostNode(requests.get(), { name: "white1", position: place })).toEqual({ title: "white1", position: place });
    expect(ghostNode(requests.get(), null)).toBeNull();
    requests.end(token, "refused");
    expect(ghostNode(requests.get(), { name: "white1", position: place })).toBeNull();
  });

  it("takes node.add's name when the place is held for no name, and says New node for a text add", () => {
    const requests = new Requests(() => 0);
    requests.begin({ method: "node.start", node: "noise" });
    expect(ghostNode(requests.get(), { name: null, position: place })).toBeNull();
    const json = requests.begin({ method: "node.add", node: "desk" });
    expect(ghostNode(requests.get(), { name: null, position: place })?.title).toBe("desk");
    requests.end(json, "answered");
    requests.begin({ method: "node.addText" });
    expect(ghostNode(requests.get(), { name: null, position: place })?.title).toBe("New node");
  });
});
