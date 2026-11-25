// A node's type actions in the inspector (docs/editor.md#actions), pure: which of them a node can take now, by its
// phase (a trigger needs its code driving, a call a configuration), and a call's arguments from its small form's
// drafts, checked by the same rules as the settings form's fields (form.ts). ear stays the judge.
import { controlOf, problemOf, shownValue, valueOf, type FieldControl, type FieldValue } from "./form";
import type { ActionArgSchema, ActionSchema, GraphNode, JsonObject, SettingSchema } from "./protocol";

/** Whether the node can take the action now, and why not when it cannot. */
export interface ActionAvailability {
  enabled: boolean;
  why?: string;
}

/** The phases in which the code drives: what a trigger needs. */
const driving = new Set(["running", "reconfiguring"]);
/** The phases in which the node has a configuration that is configured and not tearing down: what a call needs. */
const configured = new Set(["configured", "starting", "running", "stopped", "reconfiguring"]);

/** Whether `node` can take `action` now, by its phase (docs/control-port.md#actions): ear answers not-running otherwise. */
export function actionAvailability(node: GraphNode, action: ActionSchema): ActionAvailability {
  const phase: string = node.phase;
  if (action.target === "code") {
    return driving.has(phase) ? { enabled: true } : { enabled: false, why: "only while the node runs" };
  }
  return configured.has(phase) ? { enabled: true } : { enabled: false, why: "only while the node is configured" };
}

/** An argument as a setting row, so the settings form's rules read it (it is never changed: no `change`). */
function rowOf(arg: ActionArgSchema): SettingSchema {
  return { ...arg, change: "" };
}

/** One argument's field in a call's form. */
export interface ArgField {
  key: string;
  control: FieldControl;
  value: FieldValue;
  options?: string[];
  min?: number;
  max?: number;
  unit?: string;
  required: boolean;
  help?: string;
  problem?: string;
}

/** The fields of a call's arguments over `drafts` (a field with none starts at its default, or empty). */
export function argFields(action: ActionSchema, drafts: ReadonlyMap<string, FieldValue>): ArgField[] {
  return action.args.map((arg) => {
    const row = rowOf(arg);
    const control = controlOf(row);
    const value = drafts.get(arg.key) ?? shownValue(row, arg.default);
    const options =
      control === "select" && arg.values !== undefined ? (arg.required ? arg.values : ["", ...arg.values]) : undefined;
    return {
      key: arg.key,
      control,
      value,
      options,
      min: arg.min,
      max: arg.max,
      unit: arg.unit,
      required: arg.required,
      help: arg.help,
      problem: problemOf(row, value),
    };
  });
}

/** The `args` a call's drafts make, or each field's problem when any has one; an empty optional field is left out. */
export function argsOf(
  action: ActionSchema,
  drafts: ReadonlyMap<string, FieldValue>,
): { args: JsonObject } | { problems: Map<string, string> } {
  const problems = new Map<string, string>();
  const args: JsonObject = {};
  for (const field of argFields(action, drafts)) {
    if (field.problem !== undefined) {
      problems.set(field.key, field.problem);
      continue;
    }
    const arg = action.args.find((each) => each.key === field.key)!;
    const value = valueOf(rowOf(arg), field.value, undefined);
    if (value !== null) args[field.key] = value;
  }
  return problems.size > 0 ? { problems } : { args };
}

/** What a button says: its name, words for its kebab-case ("reset-clip": "Reset clip"). */
export function actionLabel(action: ActionSchema): string {
  const words = action.name.replaceAll("-", " ");
  return words.charAt(0).toUpperCase() + words.slice(1);
}
