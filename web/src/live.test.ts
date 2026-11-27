// The live controls' rules (docs/editor.md#live-numbers-faders-and-knobs), from types.json's gain schema: which rows
// are faders, knobs or numbers sent on Enter, by the features; when they are disabled; whose value they show; a family
// above 8 members as a strip; and a live number's patch and steps.
import { describe, expect, it } from "vitest";
import { fieldsOf, groupsOf, isFaderRow, liveNumberPatch, steppedValue, stripSummary, type Draft } from "./form";
import { boxKnobRows, heldByOther, liveDisabledReason, shownNumber } from "./live";
import type { GraphFile, GraphNode, TypeSchema, TypesResult } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";

const none: ReadonlyMap<string, Draft> = new Map();

function gainSchema(): TypeSchema {
  const answer = messagesOf(fixtures.types.messages).find(
    (message) => "result" in message && typeof message.result === "object" && message.result !== null && "typeSchemas" in message.result,
  ) as unknown as { result: TypesResult };
  const gain = answer.result.typeSchemas?.find((schema) => schema.name === "gain");
  if (gain === undefined) throw new Error("types.json has no gain schema");
  return gain;
}

const amp = (settings: GraphNode["settings"], gestures: GraphNode["gestures"] = []): GraphNode =>
  ({ name: "amp", type: "gain", settings, inputs: [], outputs: [], phase: "running", gestures }) as unknown as GraphNode;

const file = (over: Partial<GraphFile> = {}): GraphFile =>
  ({ version: "v1", text: "", diagnostics: [], pendingRestart: [], ...over }) as GraphFile;

describe("the live controls", () => {
  const gain = gainSchema();

  it("with gestures, live ramped decimals are faders, family members too; flags commit at once; channels is drafted", () => {
    const fields = fieldsOf(gain, { gainDb: -6 }, none, undefined, { gestures: true });
    const by = new Map(fields.map((field) => [field.key, field]));
    expect(by.get("gainDb")?.live).toBe("fader");
    expect(by.get("gainDb")?.row?.taper).toBe("db");
    expect(by.get("trimDb.1")?.live).toBe("fader");
    expect(by.get("trimDb.2")?.row?.key).toBe("trimDb.2");
    expect(by.get("mute")?.atOnce).toBe(true);
    expect(by.get("mute")?.live).toBeUndefined();
    expect(by.get("invert.1")?.atOnce).toBe(true);
    expect(by.get("channels")?.live).toBeUndefined();
    expect(by.get("channels")?.atOnce).toBeUndefined();
  });

  it("without gestures, a live decimal is a number field sent by node.set on Enter, stepping its last place", () => {
    const fields = fieldsOf(gain, {}, none);
    const gainDb = fields.find((field) => field.key === "gainDb")!;
    expect(gainDb.live).toBe("enter");
    expect(gainDb.step).toBeCloseTo(0.1, 12);
    expect(steppedValue(gainDb, "-6", 1)).toBe("-5.9");
    expect(steppedValue(gainDb, "", -1)).toBe("-0.1");
    expect(steppedValue(gainDb, "24", 1)).toBe("24.0");
    expect(steppedValue(gainDb, "-90", -1)).toBe("-90.0");
  });

  it("a live whole number steps by one, and a stepped decimal is never a fader", () => {
    const stepped: TypeSchema = {
      ...gain,
      settings: [
        { key: "bands", kind: "wholeNumber", required: false, change: "live", live: { transition: "step" }, min: 1, max: 8, default: 4 },
        { key: "knee", kind: "decimal", required: false, change: "live", live: { transition: "step" }, min: 0, max: 12, decimals: 2 },
      ],
    };
    expect(stepped.settings.map(isFaderRow)).toEqual([false, false]);
    const [bands, knee] = fieldsOf(stepped, {}, none, undefined, { gestures: true });
    expect(bands!.live).toBe("enter");
    expect(bands!.step).toBe(1);
    expect(steppedValue(bands!, "", 1)).toBe("5");
    expect(steppedValue(bands!, "8", 1)).toBe("8");
    expect(knee!.live).toBe("enter");
    expect(knee!.step).toBeCloseTo(0.01, 12);
  });

  it("a live number's patch is its number, an empty field removes the key, and a bad one is refused in ear's words", () => {
    expect(liveNumberPatch(gain, "gainDb", " -3.5 ")).toStrictEqual({ patch: { gainDb: -3.5 } });
    expect(liveNumberPatch(gain, "trimDb.2", "1")).toStrictEqual({ patch: { "trimDb.2": 1 } });
    expect(liveNumberPatch(gain, "gainDb", "")).toStrictEqual({ patch: { gainDb: null } });
    expect(liveNumberPatch(gain, "gainDb", "30")).toStrictEqual({ problem: "must be a number of dB from -90 to 24" });
    expect(liveNumberPatch(gain, "nothing", "1")).toBeUndefined();
  });

  it("the box draws a knob for each live ramped decimal outside a family when the type has one or two", () => {
    expect(boxKnobRows(gain).map((row) => row.key)).toEqual(["gainDb"]);
    expect(boxKnobRows(undefined)).toEqual([]);
    const fader = gain.settings.find((row) => row.key === "gainDb")!;
    const three: TypeSchema = { ...gain, settings: ["a", "b", "c"].map((key) => ({ ...fader, key })) };
    expect(boxKnobRows(three)).toEqual([]);
    expect(boxKnobRows({ ...three, settings: three.settings.slice(0, 2) }).map((row) => row.key)).toEqual(["a", "b"]);
  });

  it("live controls are disabled while not connected, while ear.conf has errors and while the graph refused it", () => {
    expect(liveDisabledReason("open", file())).toBeUndefined();
    expect(liveDisabledReason("open", null)).toBeUndefined();
    expect(liveDisabledReason("closed", file())).toBe("not connected to ear");
    expect(liveDisabledReason("connecting", file())).toBe("not connected to ear");
    const error = { severity: "error", code: "E1", message: "bad", line: 1, column: 1 } as const;
    const warning = { ...error, severity: "warning" } as const;
    expect(liveDisabledReason("open", file({ diagnostics: [error] }))).toBe("ear.conf has errors");
    expect(liveDisabledReason("open", file({ diagnostics: [warning] }))).toBeUndefined();
    expect(liveDisabledReason("open", file({ refusal: { code: "x", message: "no" } as never }))).toBe(
      "the graph refused ear.conf",
    );
  });

  it("a control shows its own hand, else another editor's, else the node's value, else the default", () => {
    const row = gain.settings.find((one) => one.key === "gainDb")!;
    const held = amp({ gainDb: -6 }, [{ gesture: "other", key: "gainDb", value: -20 }]);
    expect(heldByOther(held, "gainDb", "mine")).toMatchObject({ gesture: "other" });
    expect(heldByOther(held, "gainDb", "other")).toBeUndefined();
    expect(heldByOther(held, "mute", undefined)).toBeUndefined();
    expect(shownNumber(held, row, { state: "active", value: -1, id: "mine" })).toBe(-1);
    expect(shownNumber(held, row, { state: "idle" })).toBe(-20);
    expect(shownNumber(amp({ gainDb: -6 }), row, { state: "idle" })).toBe(-6);
    expect(shownNumber(amp({}), row, { state: "idle" })).toBe(0);
  });

  it("a family above 8 members is a compact strip behind a disclosure", () => {
    const eight = groupsOf(fieldsOf(gain, { channels: 8 }, none, undefined, { gestures: true }));
    expect(eight.filter((group) => group.strip)).toEqual([]);
    const nine = groupsOf(fieldsOf(gain, { channels: 9 }, none, undefined, { gestures: true }));
    const strips = nine.filter((group) => group.strip);
    expect(strips.map((group) => group.family)).toEqual(["trimDb", "invert"]);
    expect(stripSummary(strips[0]!)).toBe("trimDb.1 to trimDb.9, 9 members");
    // Every field is still drawn once, in the table's order.
    expect(nine.flatMap((group) => group.fields.map((field) => field.key))).toEqual(
      fieldsOf(gain, { channels: 9 }, none).map((field) => field.key),
    );
  });
});
