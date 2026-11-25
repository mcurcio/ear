// The top bar's words and the engine facts (docs/editor.md#the-top-bar): the pill's countdown, the engine and protocol
// chips, and the inspector's facts, on the snapshot and unknown-values fixtures' graphs.
import { describe, expect, it } from "vitest";
import type { Graph } from "./protocol";
import { connectionPill, engineChip, engineFacts, knownProtocol, nextPillChange, protocolChip } from "./shell";
import { fixtures } from "./testFixtures";

const graph = fixtures.snapshot.graph;
const closedTitle =
  "the connection to ear closed; the canvas shows the graph as ear last sent it, and the editor starts over from a " +
  "fresh snapshot when it reconnects";

describe("the connection pill", () => {
  it("says live while open and connecting… while connecting", () => {
    expect(connectionPill({ status: "open" }, 0)).toStrictEqual({
      word: "live",
      tone: "ok",
      title: "connected to ear's control port",
    });
    expect(connectionPill({ status: "connecting" }, 0)).toStrictEqual({
      word: "connecting…",
      tone: "warn",
      title: "opening the control port's socket",
    });
  });

  it("counts down to the next socket in whole seconds, never 0 s", () => {
    const now = 10_000;
    const at = (left: number) => connectionPill({ status: "closed", retryAt: now + left }, now);
    expect(at(4000)).toStrictEqual({ word: "lost: retrying in 4 s", tone: "err", title: closedTitle });
    expect(at(3200).word).toBe("lost: retrying in 4 s");
    expect(at(200).word).toBe("lost: retrying in 1 s");
    expect(at(-50).word).toBe("lost: retrying in 1 s");
    expect(connectionPill({ status: "closed" }, now)).toStrictEqual({ word: "lost", tone: "err", title: closedTitle });
  });

  it("changes next when the shown seconds drop", () => {
    const now = 10_000;
    expect(nextPillChange({ status: "closed", retryAt: now + 3200 }, now)).toBe(now + 200);
    expect(nextPillChange({ status: "closed", retryAt: now + 4000 }, now)).toBe(now + 1000);
    expect(nextPillChange({ status: "closed", retryAt: now + 800 }, now)).toBeUndefined();
    expect(nextPillChange({ status: "closed" }, now)).toBeUndefined();
    expect(nextPillChange({ status: "open" }, now)).toBeUndefined();
    expect(nextPillChange({ status: "connecting" }, now)).toBeUndefined();
  });
});

describe("the engine chip", () => {
  it("reads the rate in kHz from graphFrequency in hertz, and the block size", () => {
    expect(engineChip(graph)).toStrictEqual({
      text: "48 kHz · 256 frames",
      realtime: true,
      title: `how ear's audio thread is scheduled: ${graph.scheduling}`,
    });
    expect(engineChip({ ...graph, graphFrequency: 44100 }).text).toBe("44.1 kHz · 256 frames");
    expect(engineChip({ ...graph, realtime: false }).realtime).toBe(false);
  });
});

describe("the protocol chip", () => {
  it("shows only for a protocol newer than this editor's", () => {
    expect(knownProtocol).toBe(1);
    expect(protocolChip({ ...graph, protocol: 1 })).toBeUndefined();
    const { protocol: _protocol, ...unversioned } = graph;
    expect(protocolChip(unversioned as Graph)).toBeUndefined();
    expect(protocolChip({ ...graph, protocol: 2 })).toStrictEqual({
      text: "protocol 2",
      title: "ear speaks protocol 2, newer than this editor's 1: some of it is not shown.",
    });
  });
});

describe("the engine facts", () => {
  it("lists rate, block size, scheduling, realtime, protocol and features in order", () => {
    const facts = engineFacts(graph);
    const keys = ["rate", "blockSize", "scheduling", "realtime", "protocol", "features"];
    expect(facts.map((fact) => fact.key)).toEqual(keys);
    expect(facts.map((fact) => fact.value)).toEqual([
      "48 kHz",
      "256 frames",
      graph.scheduling,
      "yes",
      "1",
      (graph.features ?? []).join(", "),
    ]);
    expect(facts.find((fact) => fact.key === "scheduling")!.title).toBe("how ear's audio thread is scheduled");
  });

  it("marks not realtime and a newer protocol, and says none for no features", () => {
    const facts = engineFacts({ ...graph, realtime: false, protocol: 2, features: [] });
    expect(facts.find((fact) => fact.key === "realtime")).toMatchObject({ value: "no", tone: "err" });
    expect(facts.find((fact) => fact.key === "protocol")).toMatchObject({
      value: "2 (newer than this editor's 1)",
      tone: "warn",
    });
    expect(facts.find((fact) => fact.key === "features")!.value).toBe("none");
    const { features: _features, ...featureless } = graph;
    expect(engineFacts(featureless as Graph).find((fact) => fact.key === "features")!.value).toBe("none");
  });

  it("gives the unknown-values graph's rows without throwing", () => {
    const facts = engineFacts(fixtures.unknownValues.graph);
    expect(facts).toHaveLength(6);
    expect(facts.find((fact) => fact.key === "realtime")!.value).toBe("no");
  });
});
