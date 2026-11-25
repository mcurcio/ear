// The node types' schemas (types.ts) against types.json: fetched once per hash the Graph names, with the held hash as
// ifNoneMatch; notModified keeps the held set; a set is kept through the keeper and loaded before ear answers; an ear
// without the feature is never asked.
import { describe, expect, it } from "vitest";
import type { Graph, TypesResult } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";
import { TypeStore, type TypeSchemaKeeper, type TypeSchemaSet, type TypesCall } from "./types";

const messages = messagesOf(fixtures.types.messages);
const results = messages
  .filter((message) => "result" in message)
  .map((message) => (message as { result: unknown }).result);
const full = results.find((result) => typeof result === "object" && result !== null && "typeSchemas" in result) as TypesResult;
const notModified = results.find(
  (result) => typeof result === "object" && result !== null && "notModified" in result,
) as TypesResult;
const graph: Graph = fixtures.types.graph;

/** A fake ear: each types.get answered from `answers` in order, its params recorded. */
function fakeCall(answers: TypesResult[]): { call: TypesCall; asked: { ifNoneMatch?: string }[] } {
  const asked: { ifNoneMatch?: string }[] = [];
  return {
    asked,
    call: (params) => {
      asked.push(params);
      const answer = answers.shift();
      return answer === undefined ? Promise.reject(new Error("no answer")) : Promise.resolve(answer);
    },
  };
}

class MemoryKeeper implements TypeSchemaKeeper {
  constructor(public kept: TypeSchemaSet | undefined = undefined) {}
  load() {
    return this.kept;
  }
  save(set: TypeSchemaSet) {
    this.kept = set;
  }
}

describe("types.json", () => {
  it("is the schemas of the hash its Graph names, and notModified for that hash", () => {
    expect(graph.features).toContain("types");
    expect(full.hash).toBe(graph.typeSchemaHash);
    expect(notModified).toStrictEqual({ protocol: 1, hash: graph.typeSchemaHash, notModified: true });
  });
});

describe("the type store", () => {
  it("fetches the set a Graph names once, keeps it, and finds each type's schema by name", async () => {
    const keeper = new MemoryKeeper();
    const store = new TypeStore(keeper);
    let told = 0;
    store.subscribe(() => told++);
    const ear = fakeCall([full]);
    expect(store.schemaOf(graph, "rtp")).toBeUndefined();
    const first = store.sync(graph, ear.call);
    await store.sync(graph, ear.call); // a second Graph while the first is asked asks nothing
    await first;
    expect(ear.asked).toStrictEqual([{}]);
    expect(told).toBe(1);
    expect(store.schemaOf(graph, "rtp")?.settings.map((row) => row.key)).toContain("channels");
    expect(keeper.kept?.hash).toBe(graph.typeSchemaHash);
    await store.sync(graph, ear.call);
    expect(ear.asked).toHaveLength(1);
  });

  it("draws from a kept set before ear answers, and asks again only for another hash, naming the held one", async () => {
    const kept = new MemoryKeeper({ hash: full.hash, typeSchemas: full.typeSchemas ?? [] });
    const store = new TypeStore(kept);
    expect(store.schemaOf(graph, "card")).toBeDefined();
    const ear = fakeCall([notModified]);
    await store.sync(graph, ear.call);
    expect(ear.asked).toStrictEqual([]);
    const newer: Graph = { ...graph, typeSchemaHash: "fnv1a64:feedfacefeedface" };
    expect(store.schemaOf(newer, "card")).toBeUndefined(); // a set ear no longer has is not drawn
    await store.sync(newer, ear.call);
    expect(ear.asked).toStrictEqual([{ ifNoneMatch: full.hash }]);
    expect(store.get()?.hash).toBe(full.hash); // notModified kept it
  });

  it("never asks an ear without the feature, and asks again after a failure", async () => {
    const store = new TypeStore();
    const ear = fakeCall([]);
    await store.sync({ ...graph, features: ["lifecycle"] }, ear.call);
    expect(ear.asked).toStrictEqual([]);
    await store.sync(graph, ear.call); // refused: nothing held
    expect(store.get()).toBeNull();
    const again = fakeCall([full]);
    await store.sync(graph, again.call);
    expect(store.get()?.hash).toBe(full.hash);
  });
});
