// The settings form (docs/editor.md#the-settings-form), pure: fields from the schema alone, per-field drafts kept and
// marked when ear changes under them, the merge patch, and Apply asking first against a fake client
// (docs/control-port.md#nodeset-and-dry-runs): a reconfigure sent at once, a restart and a reshape asking first,
// dropRoutes sent exactly when the dry run named routes, a ports-in-use refusal asking again, a refusal placed by
// data.key. Every built-in type's schema in types.json makes a form.
import { describe, expect, it } from "vitest";
import {
  ApplyFlow,
  appliesAfter,
  asksFirst,
  confirmWords,
  edit,
  fieldsOf,
  patchOf,
  takeEar,
  type ApplyStep,
  type Draft,
  type SetClient,
} from "./form";
import type { GraphNode, Methods, RouteEnds, SetResult, TypeSchema, TypesResult } from "./protocol";
import { RpcError } from "./rpc";
import { fixtures, messagesOf } from "./testFixtures";

/** A card-like type: a required text with a pattern, two whole numbers that shape the ports, an in-place select. */
const card: TypeSchema = {
  name: "card",
  inputs: ["play"],
  outputs: ["capture"],
  reconfigureMs: 10000,
  settings: [
    { key: "card", kind: "text", required: true, change: "restart", restartReason: "card has no change in place yet", mustBe: "an ALSA card id", pattern: "^[A-Za-z0-9_-]{1,15}$", maxLength: 15 },
    { key: "playback", kind: "wholeNumber", required: false, change: "restart", min: 0, max: 64, unit: "channels", default: 0, shapes: true },
    { key: "mode", kind: "text", required: false, change: "inPlace", mustBe: "fast or slow", values: ["fast", "slow"] },
    { key: "rate", kind: "wholeNumber", required: false, change: "inPlace", min: 8000, max: 192000, unit: "Hz", defaultFrom: "graphFrequency" },
    { key: "loud", kind: "flag", required: false, change: "restart", help: "absent: quiet" },
    { key: "port", kind: "wholeNumber", required: false, change: "restart", min: 1, max: 9, onlyWithout: "mode" },
    { key: "shape", kind: "polygon", required: false, change: "sideways" },
  ],
};
const settings = { card: "MOTU", playback: 2 };
const none: ReadonlyMap<string, Draft> = new Map();

const desk: GraphNode = {
  name: "desk",
  type: "card",
  settings,
  inputs: ["play.1", "play.2"],
  outputs: [],
  state: "running",
  phase: "running",
  wanted: "running",
  blockLost: false,
};

describe("fields from the schema alone", () => {
  it("draws each row by its kind, with the node's values, its hints and what an empty field means", () => {
    const fields = fieldsOf(card, settings, none);
    expect(fields.map((field) => [field.key, field.control, field.value])).toStrictEqual([
      ["card", "text", "MOTU"],
      ["playback", "number", "2"],
      ["mode", "select", ""],
      ["rate", "number", ""],
      ["loud", "flag", false],
      ["port", "number", ""],
      ["shape", "json", ""],
    ]);
    expect(fields[0].hints).toStrictEqual(["restarts the node: card has no change in place yet"]);
    expect(fields[1].hints).toStrictEqual(["restarts the node", "changes the ports"]); // a restart with no reason given
    expect(fields[2].hints).toStrictEqual(["applies in place"]);
    expect(fields[6].hints).toStrictEqual([]); // a change this editor does not know says nothing
    expect(fields[2].options).toStrictEqual(["", "fast", "slow"]);
    expect(fields.map((field) => field.placeholder)).toStrictEqual([
      "required",
      "default 0 channels",
      "absent",
      "the graph's frequency",
      "absent",
      "absent",
      "absent",
    ]);
    expect(fields[1]).toMatchObject({ min: 0, max: 64, unit: "channels" });
    expect(fields[4].help).toBe("absent: quiet");
  });

  it("disables a row while the key it is refused beside is given, a draft included", () => {
    expect(fieldsOf(card, settings, none)[5].disabledBy).toBeUndefined();
    expect(fieldsOf(card, { ...settings, mode: "fast" }, none)[5].disabledBy).toBe("mode");
    const drafted = edit(card, settings, none, "mode", "slow");
    expect(fieldsOf(card, settings, drafted)[5].disabledBy).toBe("mode");
  });
});

describe("drafts", () => {
  it("keeps what the person typed, and drops a draft typed back to the node's value", () => {
    const typed = edit(card, settings, none, "playback", "4");
    expect(fieldsOf(card, settings, typed)[1]).toMatchObject({ value: "4", drafted: true, changedOnEar: false });
    expect(edit(card, settings, typed, "playback", "2").size).toBe(0);
    expect(edit(card, settings, none, "card", "MOTU").size).toBe(0);
  });

  it("marks a field whose value ear changed under its draft, and takes ear's value when asked", () => {
    const typed = edit(card, settings, none, "playback", "4");
    const now = { ...settings, playback: 6 };
    const field = fieldsOf(card, now, typed)[1];
    expect(field).toMatchObject({ value: "4", drafted: true, changedOnEar: true });
    // A change elsewhere leaves it unmarked.
    expect(fieldsOf(card, { ...settings, mode: "fast" }, typed)[1].changedOnEar).toBe(false);
    expect(fieldsOf(card, now, takeEar(typed, "playback"))[1]).toMatchObject({ value: "6", drafted: false });
  });

  it("says a drafted key a node changing in place is applying or will take next applies after this change", () => {
    const changing = { ...desk, phase: "reconfiguring", state: "reconfiguring", keys: ["mode"], next: ["rate"] };
    expect(appliesAfter(changing, "mode")).toBe(true);
    expect(appliesAfter(changing, "rate")).toBe(true);
    expect(appliesAfter(changing, "card")).toBe(false);
    expect(appliesAfter(desk, "mode")).toBe(false);
  });
});

describe("the patch", () => {
  it("is the drafted keys only, numbers as numbers, an emptied optional field as null", () => {
    let drafts = edit(card, settings, none, "playback", "4");
    drafts = edit(card, settings, drafts, "mode", "slow");
    drafts = edit(card, { ...settings, rate: 44100 }, drafts, "rate", "");
    drafts = edit(card, settings, drafts, "loud", true);
    drafts = edit(card, settings, drafts, "shape", '{"sides": 3}');
    expect(patchOf(card, { ...settings, rate: 44100 }, drafts)).toStrictEqual({
      patch: { playback: 4, mode: "slow", rate: null, loud: true, shape: { sides: 3 } },
    });
  });

  it("is refused before sending for a value outside the hints, each under its field", () => {
    let drafts = edit(card, settings, none, "playback", "65");
    drafts = edit(card, settings, drafts, "card", "two words");
    drafts = edit(card, settings, drafts, "port", "1.5");
    drafts = edit(card, settings, drafts, "shape", "{");
    const made = patchOf(card, settings, drafts);
    expect(made).toStrictEqual({
      problems: new Map([
        ["card", "must be an ALSA card id"],
        ["playback", "must be a whole number from 0 to 64"],
        ["port", "must be a whole number from 1 to 9"],
        ["shape", "must be JSON"],
      ]),
    });
    expect(fieldsOf(card, settings, drafts)[1].problem).toBe("must be a whole number from 0 to 64");
    expect(patchOf(card, settings, edit(card, settings, none, "card", ""))).toStrictEqual({
      problems: new Map([["card", "required"]]),
    });
  });

  it("places ear's refusal under the field its data.key names", () => {
    const fields = fieldsOf(card, settings, none, { key: "mode", detail: "must be fast or slow" });
    expect(fields[2].problem).toBe("must be fast or slow");
    expect(fields.filter((field) => field.problem !== undefined)).toHaveLength(1);
  });
});

/** A fake ear: answers each node.set with the next scripted answer, recording what it was sent. */
class FakeEar implements SetClient {
  sent: Methods["node.set"]["params"][] = [];
  constructor(private readonly answers: (SetResult | RpcError)[]) {}
  set(params: Methods["node.set"]["params"]): Promise<SetResult> {
    this.sent.push(params);
    const answer = this.answers.shift();
    if (answer === undefined) return Promise.reject(new Error("no answer scripted"));
    return answer instanceof RpcError ? Promise.reject(answer) : Promise.resolve(answer);
  }
}

const stranded: RouteEnds = { from: { node: "kitchen", port: "from.left" }, to: { node: "desk", port: "play.2" } };

function flowOf(ear: FakeEar, dryRun = true): { flow: ApplyFlow; steps: ApplyStep["kind"][] } {
  const steps: ApplyStep["kind"][] = [];
  return { flow: new ApplyFlow("desk", ear, dryRun, (step) => steps.push(step.kind)), steps };
}

describe("Apply asks first", () => {
  it("sends a reconfigure at once after its dry run", async () => {
    const ear = new FakeEar([{ applied: "reconfigure" }, { applied: "reconfigure" }]);
    const { flow, steps } = flowOf(ear);
    await flow.apply({ mode: "fast" });
    expect(ear.sent).toStrictEqual([
      { name: "desk", settings: { mode: "fast" }, dryRun: true },
      { name: "desk", settings: { mode: "fast" } },
    ]);
    expect(steps).toStrictEqual(["checking", "sending", "applied"]);
    expect(flow.step()).toStrictEqual({ kind: "applied", applied: "reconfigure" });
  });

  it("asks first for a restart, and sends it once confirmed, without dropRoutes", async () => {
    const ear = new FakeEar([{ applied: "restart" }, { applied: "restart" }]);
    const { flow } = flowOf(ear);
    await flow.apply({ card: "Pro24" });
    expect(flow.step()).toStrictEqual({
      kind: "confirm",
      words: "Restarts desk: its audio stops for a moment.",
      routes: [],
      dropRoutes: false,
    });
    expect(ear.sent).toHaveLength(1);
    await flow.confirm();
    expect(ear.sent[1]).toStrictEqual({ name: "desk", settings: { card: "Pro24" } });
    expect(flow.step()).toStrictEqual({ kind: "applied", applied: "restart" });
  });

  it("asks first for a reshape naming its routes, and the confirmed send drops them", async () => {
    const dry: SetResult = {
      applied: "reshape",
      inPlace: false,
      ports: { added: [], removed: ["play.2"] },
      routes: [stranded],
    };
    const ear = new FakeEar([dry, { applied: "reshape", inPlace: false, routesDropped: [stranded] }]);
    const { flow } = flowOf(ear);
    await flow.apply({ playback: 1 });
    expect(flow.step()).toStrictEqual({
      kind: "confirm",
      words: "Restarts desk and removes 1 route: kitchen:from.left → desk:play.2",
      routes: [stranded],
      dropRoutes: true,
    });
    await flow.confirm();
    expect(ear.sent[1]).toStrictEqual({ name: "desk", settings: { playback: 1 }, dropRoutes: true });
  });

  it("asks first for a reshape in place that strands nothing, in words that stop nothing", () => {
    expect(asksFirst({ applied: "reshape", inPlace: true })).toBe(true);
    expect(confirmWords("fpp", { applied: "reshape", inPlace: true })).toBe("Changes fpp's ports.");
    expect(asksFirst({ applied: "unchanged" })).toBe(false);
    expect(asksFirst({ applied: "stored" })).toBe(false);
    expect(asksFirst({ applied: "teleport" })).toBe(true); // an outcome this editor does not know asks
  });

  it("sends nothing when the person cancels", async () => {
    const ear = new FakeEar([{ applied: "restart" }]);
    const { flow } = flowOf(ear);
    await flow.apply({ card: "Pro24" });
    flow.cancel();
    expect(flow.step()).toStrictEqual({ kind: "idle" });
    await flow.confirm();
    expect(ear.sent).toHaveLength(1);
  });

  it("asks again with the refusal's routes when another edit stranded a route between", async () => {
    const inUse = new RpcError({
      code: -32000,
      message: "ports-in-use desk: play.2 (1 route)",
      data: { code: "ports-in-use", ports: ["play.2"], routes: [stranded] },
    });
    const ear = new FakeEar([{ applied: "reshape", inPlace: false }, inUse, { applied: "reshape", inPlace: false }]);
    const { flow } = flowOf(ear);
    await flow.apply({ playback: 1 });
    await flow.confirm();
    expect(ear.sent[1]).toStrictEqual({ name: "desk", settings: { playback: 1 } }); // the dry run named no routes
    expect(flow.step()).toMatchObject({ kind: "confirm", routes: [stranded], dropRoutes: true });
    await flow.confirm();
    expect(ear.sent[2]).toStrictEqual({ name: "desk", settings: { playback: 1 }, dropRoutes: true });
    expect(flow.step()).toStrictEqual({ kind: "applied", applied: "reshape" });
  });

  it("places a refusal by its data.key, at the dry run or the send", async () => {
    const invalid = new RpcError({
      code: -32000,
      message: "invalid-settings desk: playback: must be a whole number of channels, 0-64",
      data: { code: "invalid-settings", key: "playback", detail: "must be a whole number of channels, 0-64" },
    });
    const ear = new FakeEar([invalid]);
    const { flow } = flowOf(ear);
    await flow.apply({ playback: 99 });
    expect(flow.step()).toMatchObject({
      kind: "refused",
      key: "playback",
      detail: "must be a whole number of channels, 0-64",
    });
    const keyless = new RpcError({
      code: -32000,
      message: "invalid-settings desk: playback or capture: at least one must be above 0",
      data: { code: "invalid-settings", detail: "playback or capture: at least one must be above 0" },
    });
    const other = flowOf(new FakeEar([{ applied: "restart" }, keyless]));
    await other.flow.apply({ playback: 0 });
    await other.flow.confirm();
    expect(other.flow.step()).toMatchObject({ kind: "refused", detail: "playback or capture: at least one must be above 0" });
    expect("key" in other.flow.step()).toBe(false);
  });

  it("sends at once without the dryRun feature", async () => {
    const ear = new FakeEar([{ applied: "restart" }]);
    const { flow, steps } = flowOf(ear, false);
    await flow.apply({ card: "Pro24" });
    expect(ear.sent).toStrictEqual([{ name: "desk", settings: { card: "Pro24" } }]);
    expect(steps).toStrictEqual(["sending", "applied"]);
  });

  it("answers node-set.json's dry runs as ear did: a reconfigure at once, every other asking", () => {
    const results = messagesOf(fixtures.nodeSet.messages)
      .filter((message) => "result" in message && typeof message.result === "object" && message.result !== null)
      .map((message) => (message as unknown as { result: SetResult }).result)
      .filter((result) => typeof result.applied === "string");
    expect(results.length).toBeGreaterThan(0);
    for (const result of results) {
      const routes = result.routes ?? [];
      const stops = result.applied === "restart" || (result.applied === "reshape" && result.inPlace !== true);
      expect(asksFirst(result)).toBe(!["reconfigure", "unchanged", "stored"].includes(result.applied) || routes.length > 0);
      if (stops && routes.length === 0) expect(confirmWords("n", result)).toBe("Restarts n: its audio stops for a moment.");
    }
  });
});

describe("every built-in type makes a form", () => {
  it("from types.json's schemas, each row a field and every hint in words", () => {
    const answer = messagesOf(fixtures.types.messages).find(
      (message) => "result" in message && typeof message.result === "object" && message.result !== null && "typeSchemas" in message.result,
    ) as { result: TypesResult } | undefined;
    expect(answer).toBeDefined();
    const schemas = answer!.result.typeSchemas ?? [];
    expect(schemas.map((schema) => schema.name)).toContain("rtp");
    for (const schema of schemas) {
      const fields = fieldsOf(schema, {}, none);
      expect(fields.map((field) => field.key)).toStrictEqual(schema.settings.map((row) => row.key));
      for (const field of fields) {
        expect(field.control).not.toBe("json"); // ear's kinds today are all known here
        expect(field.hints.length).toBeGreaterThan(0);
      }
    }
  });
});
