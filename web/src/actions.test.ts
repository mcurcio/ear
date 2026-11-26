// A node's type actions (actions.ts, docs/editor.md#actions): which a node can take by its phase, and a call's
// arguments from its form's drafts.
import { describe, expect, it } from "vitest";
import { actionAvailability, actionLabel, argFields, argsOf } from "./actions";
import type { ActionSchema, GraphNode, NodePhase } from "./protocol";

const reset: ActionSchema = { name: "reset-clip", by: "type", target: "code", help: "clear the clipping flag", args: [] };
const add: ActionSchema = {
  name: "add",
  by: "type",
  target: "loop",
  help: "add to the count",
  args: [
    { key: "amount", kind: "wholeNumber", required: true, min: 1, max: 10 },
    { key: "note", kind: "text", required: false, mustBe: "a word" },
  ],
};

function nodeAt(phase: NodePhase): GraphNode {
  return {
    name: "amp",
    type: "gain",
    settings: {},
    inputs: [],
    outputs: [],
    state: "running",
    phase,
    wanted: "running",
    blockLost: false,
  } as GraphNode;
}

describe("a node's actions", () => {
  it("takes a trigger only while its code drives, and a call while it is configured, stopped included", () => {
    for (const phase of ["running", "reconfiguring"] as NodePhase[])
      expect(actionAvailability(nodeAt(phase), reset).enabled, phase).toBe(true);
    for (const phase of ["added", "configuring", "configured", "starting", "stopping", "stopped", "failed", "disabled"] as NodePhase[])
      expect(actionAvailability(nodeAt(phase), reset).enabled, phase).toBe(false);
    for (const phase of ["configured", "starting", "running", "stopped", "reconfiguring"] as NodePhase[])
      expect(actionAvailability(nodeAt(phase), add).enabled, phase).toBe(true);
    for (const phase of ["added", "configuring", "stopping", "failed", "disabled"] as NodePhase[]) {
      const availability = actionAvailability(nodeAt(phase), add);
      expect(availability.enabled, phase).toBe(false);
      expect(availability.why).toBe("only while the node is configured");
    }
  });

  it("starts a flag argument at its default and sends it unchecked when it defaults to true", () => {
    const loud: ActionSchema = {
      name: "play",
      by: "type",
      target: "code",
      help: "play a tone",
      args: [{ key: "loud", kind: "flag", required: false, default: true }],
    };
    expect(argFields(loud, new Map()).map((field) => field.value)).toEqual([true]);
    expect(argsOf(loud, new Map())).toEqual({ args: {} });
    expect(argsOf(loud, new Map([["loud", false]]))).toEqual({ args: { loud: false } });
  });

  it("draws a call's arguments as setting fields and makes its args, an empty optional one left out", () => {
    const fields = argFields(add, new Map());
    expect(fields.map((field) => [field.key, field.control, field.problem])).toEqual([
      ["amount", "number", "required"],
      ["note", "text", undefined],
    ]);
    expect(argsOf(add, new Map([["amount", "11"]]))).toEqual({
      problems: new Map([["amount", "must be a whole number from 1 to 10"]]),
    });
    expect(argsOf(add, new Map([["amount", "3"]]))).toEqual({ args: { amount: 3 } });
    expect(argsOf(add, new Map([["amount", "3"], ["note", "hi"]]))).toEqual({ args: { amount: 3, note: "hi" } });
  });

  it("labels a button with its name in words", () => {
    expect(actionLabel(reset)).toBe("Reset clip");
    expect(actionLabel(add)).toBe("Add");
  });
});
