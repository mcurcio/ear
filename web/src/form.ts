// The settings form (docs/editor.md#the-settings-form), pure: each field drawn from the node type's schema alone (no
// type is named here), its per-field draft, the merge patch Apply sends, and the Apply flow that asks first
// (docs/control-port.md#nodeset-and-dry-runs): a dry run of node.set, then the real one at once for a change that
// stops nothing, or after the person confirms one that restarts the node, reshapes it or removes routes. ear stays the
// judge: the hints here only help a person type a value ear will take, and every refusal is ear's.
import type {
  Applied,
  GraphNode,
  JsonObject,
  JsonValue,
  Methods,
  RouteEnds,
  SetResult,
  SettingSchema,
  TypeSchema,
} from "./protocol";
import { RpcError } from "./rpc";
import { wireEnds } from "./wires";

// ----- Fields -----

/** How a field is drawn: by its row's kind and hints; a kind this editor does not know is typed as JSON. */
export type FieldControl = "select" | "text" | "number" | "decimal" | "flag" | "json";

/** What a field shows while no draft is typed: the node's value as text (a flag as a boolean). */
export type FieldValue = string | boolean;

/** One field's draft: what the person typed, and the node's value when they began, to see ear change under it. */
export interface Draft {
  value: FieldValue;
  base: JsonValue | undefined;
}

/** Every field's draft, by key; a field with none shows the node's value. */
export type Drafts = ReadonlyMap<string, Draft>;

/** One field as the inspector draws it. */
export interface FieldView {
  key: string;
  control: FieldControl;
  /** The draft, or the node's value. */
  value: FieldValue;
  /** A select's values; for an optional row the first is "" (absent). */
  options?: string[];
  /** A number's or a decimal's bounds, or a text's most characters. */
  min?: number;
  max?: number;
  maxLength?: number;
  unit?: string;
  /** What an empty field means: the default, or where it comes from. */
  placeholder: string;
  required: boolean;
  /** The words beside the field, from the schema alone: "applies in place", "restarts the node[: <its reason>]",
   * "changes the ports". */
  hints: string[];
  help?: string;
  /** Disabled while this other key is given (its row's onlyWithout). */
  disabledBy?: string;
  /** The person typed in it. */
  drafted: boolean;
  /** ear's value changed under the draft: offer to take ear's (docs/editor.md#the-settings-form). */
  changedOnEar: boolean;
  /** A family member's: its family's key and its number, from 1. */
  family?: string;
  member?: number;
  /** A family member above its count row's value: kept, not used (drawn greyed). */
  inactive?: boolean;
  /** A live flag or select: a change of it is sent at once by node.set, with no draft and no Apply. */
  atOnce?: true;
  /**
   * A live number (docs/editor.md#live-numbers-faders-and-knobs), with no draft: "fader", a live ramped decimal with
   * the gestures feature, dragged as a gesture with a number field beside it; "enter", any other live whole number or
   * decimal, steppers and a number field sent by node.set on Enter or a step.
   */
  live?: "fader" | "enter";
  /** A live "enter" field's step: 1 for a whole number, one of its last places for a decimal. */
  step?: number;
  /** The row the field reads (a family member's with its own key), for a live control's taper and words. */
  row?: SettingSchema;
  /** Why the draft cannot be sent as it is (checked here; ear still judges), or ear's refusal placed here. */
  problem?: string;
}

/** The control a row is drawn with. */
export function controlOf(row: SettingSchema): FieldControl {
  if (row.kind === "text") return row.values !== undefined && row.values.length > 0 ? "select" : "text";
  // A closed set (docs/control-port.md#type-schemas): a select of its names, and nothing else is sent.
  if (row.kind === "choice") return "select";
  if (row.kind === "wholeNumber") return "number";
  if (row.kind === "decimal") return "decimal";
  if (row.kind === "flag") return "flag";
  return "json";
}

/** The node's value of `row` as its field shows it; absent is "" (a flag: its default, else false). */
export function shownValue(row: SettingSchema, value: JsonValue | undefined): FieldValue {
  const control = controlOf(row);
  if (control === "flag") return typeof value === "boolean" ? value : row.default === true;
  if (value === undefined || value === null) return "";
  if (control === "json") return JSON.stringify(value);
  return typeof value === "string" ? value : String(value);
}

/** The words beside a field (docs/editor.md#the-settings-form); a `change` this editor does not know says nothing. */
export function hintsOf(row: SettingSchema): string[] {
  const hints: string[] = [];
  if (row.change === "live") hints.push(row.live?.transition === "ramp" ? "live, ramped" : "live");
  if (row.change === "inPlace") hints.push("applies in place");
  if (row.change === "restart")
    hints.push(row.restartReason !== undefined ? `restarts the node: ${row.restartReason}` : "restarts the node");
  if (row.shapes === true) hints.push("changes the ports");
  return hints;
}

/** What an empty field means. */
export function placeholderOf(row: SettingSchema): string {
  if (row.required) return "required";
  if (row.default !== undefined) return `default ${String(row.default)}${row.unit !== undefined ? ` ${row.unit}` : ""}`;
  if (row.defaultFrom === "graphFrequency") return "the graph's frequency";
  if (row.defaultFrom === "nodeName") return "the node's name";
  return "absent";
}

// ----- Indexed families -----

/**
 * A family member's number in `key`, by the one grammar (docs/control-port.md#type-schemas): `<family>.<n>`, n in
 * decimal from 1, no sign and no leading zero; undefined for any other spelling.
 */
export function memberNumber(key: string, family: string): number | undefined {
  if (!key.startsWith(`${family}.`)) return undefined;
  const digits = key.slice(family.length + 1);
  if (!/^[1-9]\d*$/.test(digits)) return undefined;
  return Number(digits);
}

/** The row `key` is read by: a plain row's own key, or a family's member within its range; undefined for none. */
export function rowOfKey(schema: TypeSchema, key: string): SettingSchema | undefined {
  for (const row of schema.settings) {
    if (row.indexed === undefined) {
      if (row.key === key) return row;
      continue;
    }
    const member = memberNumber(key, row.key);
    if (member !== undefined && member >= row.indexed.from && member <= row.indexed.max) return memberRow(row, member);
  }
  return undefined;
}

/** A family's row as one member's field reads it: the member's key, the family's rule. */
function memberRow(family: SettingSchema, member: number): SettingSchema {
  const row: SettingSchema = { ...family, key: `${family.key}.${member}` };
  delete row.indexed;
  return row;
}

/**
 * A count row's value as the form has it: the draft when it is a whole number, else the node's, else its default; a
 * cleared draft removes the key, so it reads the default. A draft still being typed ("4.") keeps the node's count.
 */
function countOf(schema: TypeSchema, by: string, settings: JsonObject, drafts: Drafts): number {
  const draft = drafts.get(by);
  const typed = draft === undefined ? undefined : String(draft.value).trim();
  if (typed !== undefined && /^\d+$/.test(typed)) return Number(typed);
  const value = typed === "" ? undefined : settings[by];
  if (typeof value === "number") return value;
  const row = schema.settings.find((one) => one.key === by);
  return typeof row?.default === "number" ? row.default : 0;
}

/**
 * Every row as its fields: a plain row once, a family as members 1 to its count row's value, and any member above it
 * that the node or a draft gives, kept and marked inactive (docs/editor.md#the-settings-form).
 */
function expandedRows(
  schema: TypeSchema,
  settings: JsonObject,
  drafts: Drafts,
): Array<{ row: SettingSchema; family?: SettingSchema; member?: number; inactive?: boolean; count?: number }> {
  const expanded: Array<{ row: SettingSchema; family?: SettingSchema; member?: number; inactive?: boolean; count?: number }> =
    [];
  for (const row of schema.settings) {
    if (row.indexed === undefined) {
      expanded.push({ row });
      continue;
    }
    const count = Math.min(countOf(schema, row.indexed.by, settings, drafts), row.indexed.max);
    let last = count;
    for (const key of [...Object.keys(settings), ...drafts.keys()]) {
      const member = memberNumber(key, row.key);
      if (member !== undefined && member <= row.indexed.max) last = Math.max(last, member);
    }
    for (let member = row.indexed.from; member <= last; ++member)
      expanded.push({ row: memberRow(row, member), family: row, member, inactive: member > count, count });
  }
  return expanded;
}

/** What the form draws by the features ear has: faders only with gestures. */
export interface FormOptions {
  gestures?: boolean;
}

/**
 * Whether a row is dragged as a gesture: a live ramped decimal (docs/control-port.md#gestures), the only rows a
 * gesture moves; a stepped one has no value between two to drag through.
 */
export function isFaderRow(row: SettingSchema): boolean {
  return row.change === "live" && row.live?.transition === "ramp" && row.kind === "decimal";
}

/** How a live row's number is drawn, by the features: undefined for a row that is not a live number. */
export function liveNumberOf(row: SettingSchema, options: FormOptions): "fader" | "enter" | undefined {
  if (row.change !== "live" || (row.kind !== "decimal" && row.kind !== "wholeNumber")) return undefined;
  return isFaderRow(row) && options.gestures === true ? "fader" : "enter";
}

/** The fields of `schema` over the node's `settings`, with `drafts`; `refusal` is ear's last, placed under its key. */
export function fieldsOf(
  schema: TypeSchema,
  settings: JsonObject,
  drafts: Drafts,
  refusal?: { key: string; detail: string },
  options: FormOptions = {},
): FieldView[] {
  return expandedRows(schema, settings, drafts).map(({ row, family, member, inactive, count }) => {
    const draft = drafts.get(row.key);
    const control = controlOf(row);
    const hints = hintsOf(row);
    if (inactive === true && family?.indexed !== undefined)
      hints.unshift(`kept; not used while ${family.indexed.by} is ${count ?? 0}`);
    const field: FieldView = {
      key: row.key,
      control,
      value: draft !== undefined ? draft.value : shownValue(row, settings[row.key]),
      placeholder: placeholderOf(row),
      required: row.required,
      hints,
      drafted: draft !== undefined,
      changedOnEar: draft !== undefined && !sameJson(draft.base, settings[row.key]),
    };
    if (family !== undefined && member !== undefined) {
      field.family = family.key;
      field.member = member;
      if (inactive === true) field.inactive = true;
    }
    if (commitsAtOnce(row)) field.atOnce = true;
    const live = liveNumberOf(row, options);
    if (live !== undefined) {
      field.live = live;
      field.row = row;
      field.step = row.kind === "wholeNumber" ? 1 : 10 ** -(row.decimals ?? 2);
    }
    if (control === "select") field.options = row.required ? [...(row.values ?? [])] : ["", ...(row.values ?? [])];
    if (control === "number" || control === "decimal") {
      if (row.min !== undefined) field.min = row.min;
      if (row.max !== undefined) field.max = row.max;
    }
    if (row.maxLength !== undefined) field.maxLength = row.maxLength;
    if (row.unit !== undefined) field.unit = row.unit;
    if (row.help !== undefined) field.help = row.help;
    if (row.onlyWithout !== undefined && given(row.onlyWithout, settings, drafts)) field.disabledBy = row.onlyWithout;
    const problem = draft !== undefined ? problemOf(row, draft.value) : undefined;
    if (problem !== undefined) field.problem = problem;
    else if (refusal !== undefined && refusal.key === row.key) field.problem = refusal.detail;
    return field;
  });
}

/** A family drawn behind a disclosure, as a compact strip, above this many members. */
export const stripAbove = 8;

/** The fields in groups: each plain field alone, each family's members together, a strip when above stripAbove. */
export interface FieldGroup {
  /** The family's key, for a family's members. */
  family?: string;
  fields: FieldView[];
  /** Drawn as a compact strip behind a disclosure. */
  strip: boolean;
}

export function groupsOf(fields: FieldView[]): FieldGroup[] {
  const groups: FieldGroup[] = [];
  for (const field of fields) {
    const last = groups[groups.length - 1];
    if (field.family !== undefined && last?.family === field.family) last.fields.push(field);
    else groups.push(field.family !== undefined ? { family: field.family, fields: [field], strip: false } : { fields: [field], strip: false });
  }
  for (const group of groups) group.strip = group.family !== undefined && group.fields.length > stripAbove;
  return groups;
}

/** A strip's disclosure: "trimDb.1 to trimDb.16, 16 members". */
export function stripSummary(group: FieldGroup): string {
  const first = group.fields[0]?.key ?? "";
  const last = group.fields[group.fields.length - 1]?.key ?? "";
  return `${first} to ${last}, ${group.fields.length} members`;
}

/** Whether `key` will be given once the drafts are applied. */
function given(key: string, settings: JsonObject, drafts: Drafts): boolean {
  const draft = drafts.get(key);
  if (draft === undefined) return settings[key] !== undefined && settings[key] !== null;
  return draft.value !== "" && draft.value !== false;
}

function sameJson(a: JsonValue | undefined, b: JsonValue | undefined): boolean {
  return JSON.stringify(a ?? null) === JSON.stringify(b ?? null);
}

// ----- Drafts -----

/** The drafts after the person typed `value` in `key`'s field: a value equal to the node's drops the draft. */
export function edit(
  schema: TypeSchema,
  settings: JsonObject,
  drafts: Drafts,
  key: string,
  value: FieldValue,
): Map<string, Draft> {
  const next = new Map(drafts);
  const row = rowOfKey(schema, key);
  if (row === undefined) return next;
  const before = drafts.get(key);
  if (before === undefined && value === shownValue(row, settings[key])) return next;
  if (before !== undefined && sameJson(before.base, settings[key]) && value === shownValue(row, settings[key])) {
    next.delete(key);
    return next;
  }
  next.set(key, { value, base: before !== undefined ? before.base : settings[key] });
  return next;
}

/** The drafts without `key`'s: the field shows ear's value again ("take ear's value"). */
export function takeEar(drafts: Drafts, key: string): Map<string, Draft> {
  const next = new Map(drafts);
  next.delete(key);
  return next;
}

/** What is wrong with `value` for `row` before it is sent, if anything: ear's range and hints, in ear's words. */
export function problemOf(row: SettingSchema, value: FieldValue): string | undefined {
  const control = controlOf(row);
  if (control === "flag") return undefined;
  const text = String(value);
  if (text === "") return row.required ? "required" : undefined;
  if (control === "number") {
    const number = Number(text.trim());
    const range = row.min !== undefined && row.max !== undefined ? ` from ${row.min} to ${row.max}` : "";
    if (!/^-?\d+$/.test(text.trim()) || !Number.isSafeInteger(number)) return `must be a whole number${range}`;
    if ((row.min !== undefined && number < row.min) || (row.max !== undefined && number > row.max))
      return `must be a whole number${range}`;
    return undefined;
  }
  if (control === "decimal") {
    // ear's own words (docs/writing-a-node-type.md#decimal-rows): a JSON number, an integer too, in the range.
    const unit = row.unit !== undefined ? ` of ${row.unit}` : "";
    const range = row.min !== undefined && row.max !== undefined ? ` from ${row.min} to ${row.max}` : "";
    const words = `must be a number${unit}${range}`;
    const trimmed = text.trim();
    if (!/^-?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?$/.test(trimmed)) return words;
    const number = Number(trimmed);
    if (!Number.isFinite(number)) return words;
    if ((row.min !== undefined && number < row.min) || (row.max !== undefined && number > row.max)) return words;
    return undefined;
  }
  if (control === "json") {
    try {
      JSON.parse(text);
      return undefined;
    } catch {
      return "must be JSON";
    }
  }
  if (row.kind === "choice") {
    // ear's own words (docs/writing-a-node-type.md#choice-rows): one of its names, exactly.
    const names = row.values ?? [];
    return names.includes(text) ? undefined : `must be one of ${quotedList(names)}`;
  }
  const mustBe = row.mustBe !== undefined ? `must be ${row.mustBe}` : "is not taken";
  if (row.maxLength !== undefined && new TextEncoder().encode(text).length > row.maxLength) return mustBe;
  if (row.pattern !== undefined) {
    try {
      if (!new RegExp(row.pattern).test(text)) return mustBe;
    } catch {
      // A pattern this browser cannot read is no hint: ear judges.
    }
  }
  return undefined;
}

/** `"a", "b" and "c"`, as ear lists names in a refusal. */
function quotedList(names: string[]): string {
  const quoted = names.map((name) => `"${name}"`);
  return quoted.length <= 1 ? quoted.join("") : `${quoted.slice(0, -1).join(", ")} and ${quoted[quoted.length - 1]}`;
}

/**
 * The value `draft` sends for `row`: an empty optional field removes the key, and so does an optional flag left at what
 * its absence reads (its default, else false) when the node gives none; a flag set against its default is sent.
 */
export function valueOf(row: SettingSchema, draft: FieldValue, current: JsonValue | undefined): JsonValue {
  const control = controlOf(row);
  if (control === "flag")
    return draft === (row.default === true) && !row.required && current === undefined ? null : draft;
  const text = String(draft);
  if (text === "") return null;
  if (control === "number" || control === "decimal") return Number(text.trim());
  if (control === "json") return JSON.parse(text) as JsonValue;
  return text;
}

/** The merge patch the drafts make (node.set's settings), or each field's problem when any has one. */
export function patchOf(
  schema: TypeSchema,
  settings: JsonObject,
  drafts: Drafts,
): { patch: JsonObject } | { problems: Map<string, string> } {
  const patch: JsonObject = {};
  const problems = new Map<string, string>();
  for (const [key, draft] of drafts) {
    const row = rowOfKey(schema, key);
    if (row === undefined) continue;
    const problem = problemOf(row, draft.value);
    if (problem !== undefined) problems.set(key, problem);
    else patch[key] = valueOf(row, draft.value, settings[key]);
  }
  return problems.size > 0 ? { problems } : { patch };
}

/**
 * A node changing in place says, beside a drafted key it is applying now or will take next, that the edit applies
 * after this change (docs/editor.md#the-settings-form).
 */
export function appliesAfter(node: GraphNode, key: string): boolean {
  return node.phase === "reconfiguring" && ((node.keys ?? []).includes(key) || (node.next ?? []).includes(key));
}

// ----- Live switches and selects -----

/**
 * Whether a row's field commits at once (docs/editor.md#the-settings-form): a live flag or select is one click, sent by
 * node.set as it changes, with no draft, dry run or Apply; ear retunes it with no restart.
 */
export function commitsAtOnce(row: SettingSchema): boolean {
  const control = controlOf(row);
  return row.change === "live" && (control === "flag" || control === "select");
}

/** The node.set patch of one at-once field's new value, or its problem; undefined for a key the schema lacks. */
export function atOncePatch(
  schema: TypeSchema,
  settings: JsonObject,
  key: string,
  value: FieldValue,
): { patch: JsonObject } | { problem: string } | undefined {
  const row = rowOfKey(schema, key);
  if (row === undefined) return undefined;
  const problem = problemOf(row, value);
  if (problem !== undefined) return { problem };
  return { patch: { [key]: valueOf(row, value, settings[key]) } };
}

/**
 * The node.set patch of a live number's typed text (Enter, or a step), or its problem; an empty field removes the key.
 * Undefined for a key the schema lacks.
 */
export function liveNumberPatch(
  schema: TypeSchema,
  key: string,
  text: string,
): { patch: JsonObject } | { problem: string } | undefined {
  const row = rowOfKey(schema, key);
  if (row === undefined) return undefined;
  const problem = problemOf(row, text);
  if (problem !== undefined) return { problem };
  return { patch: { [key]: text.trim() === "" ? null : Number(text.trim()) } };
}

/** A live number stepped by `by` steps from `value` (the node's), within its range, at the step's places. */
export function steppedValue(field: FieldView, value: string, by: number): string {
  const step = field.step ?? 1;
  const current = Number(value.trim() === "" ? (field.row?.default ?? field.min ?? 0) : value.trim());
  const base = Number.isFinite(current) ? current : 0;
  let next = Math.round((base + by * step) / step) * step;
  if (field.min !== undefined && next < field.min) next = field.min;
  if (field.max !== undefined && next > field.max) next = field.max;
  const places = step >= 1 ? 0 : Math.round(-Math.log10(step));
  return next.toFixed(places);
}

/**
 * The notice after a change that moved other nodes through a shared definition (node.set's alsoChanged): "also changed
 * amp2, amp3", each node once in ear's order; undefined when it moved none.
 */
export function alsoChangedWords(result: SetResult): string | undefined {
  const names: string[] = [];
  for (const one of result.alsoChanged ?? []) if (!names.includes(one.name)) names.push(one.name);
  return names.length > 0 ? `also changed ${names.join(", ")}` : undefined;
}

// ----- Apply asks first -----

/** What the Apply flow needs of the editor: node.set, answered or refused. */
export interface SetClient {
  set(params: Methods["node.set"]["params"]): Promise<SetResult>;
}

/** Where an Apply is. */
export type ApplyStep =
  | { kind: "idle" }
  /** The dry run is on its way. */
  | { kind: "checking" }
  /** The person is asked: `words` says what will happen, and confirming sends `dropRoutes` when routes would go. */
  | { kind: "confirm"; words: string; routes: RouteEnds[]; dropRoutes: boolean }
  /** The real node.set is on its way. */
  | { kind: "sending" }
  | { kind: "applied"; applied: Applied }
  /** ear refused: under the field its data.key names, else at the form's top. */
  | { kind: "refused"; error: unknown; key?: string; detail: string };

/** Whether a dry run's answer needs the person's yes: a restart, a reshape, routes removed, or an outcome unknown. */
export function asksFirst(result: SetResult): boolean {
  if ((result.routes ?? []).length > 0) return true;
  return !["reconfigure", "unchanged", "stored", "retune"].includes(result.applied);
}

/** The question for a dry run's answer: "Restarts desk: its audio stops for a moment.", or the routes it removes. */
export function confirmWords(node: string, result: SetResult): string {
  const routes = result.routes ?? [];
  const restarts = result.applied === "restart" || (result.applied === "reshape" && result.inPlace !== true);
  const what = restarts ? `Restarts ${node}` : result.applied === "reshape" ? `Changes ${node}'s ports` : `Changes ${node}`;
  if (routes.length === 0) return restarts ? `${what}: its audio stops for a moment.` : `${what}.`;
  const count = `${routes.length} route${routes.length === 1 ? "" : "s"}`;
  return `${what} and removes ${count}: ${routes.map(wireEnds).join(", ")}`;
}

function refusedStep(error: unknown): ApplyStep {
  const data = error instanceof RpcError ? error.data : undefined;
  const detail = data?.detail ?? (error instanceof Error ? error.message : String(error));
  return data?.key !== undefined ? { kind: "refused", error, key: data.key, detail } : { kind: "refused", error, detail };
}

/**
 * One node's Apply (docs/control-port.md#nodeset-and-dry-runs): with `dryRun`, node.set with dryRun first; a change
 * that stops nothing is then sent at once, any other waits for confirm() (or cancel()). The confirmed send carries
 * dropRoutes exactly when the dry run named routes; a ports-in-use refusal (another edit landed between) asks again
 * with the refusal's routes. Without `dryRun` it sends at once. `onStep` hears every step.
 */
export class ApplyFlow {
  private current: ApplyStep = { kind: "idle" };
  private pending: { patch: JsonObject; dropRoutes: boolean } | null = null;

  constructor(
    private readonly node: string,
    private readonly client: SetClient,
    private readonly dryRun: boolean,
    private readonly onStep: (step: ApplyStep) => void = () => {},
  ) {}

  step(): ApplyStep {
    return this.current;
  }

  /** Sends `patch` the way the flow says; settles once the flow rests (applied, refused or asking). */
  async apply(patch: JsonObject): Promise<void> {
    if (!this.dryRun) return this.send(patch, false);
    this.move({ kind: "checking" });
    let result: SetResult;
    try {
      result = await this.client.set({ name: this.node, settings: patch, dryRun: true });
    } catch (error) {
      return this.move(refusedStep(error));
    }
    if (!asksFirst(result)) return this.send(patch, false);
    const routes = result.routes ?? [];
    this.ask(patch, confirmWords(this.node, result), routes);
  }

  /** The person said yes. */
  async confirm(): Promise<void> {
    if (this.current.kind !== "confirm" || this.pending === null) return;
    const { patch, dropRoutes } = this.pending;
    return this.send(patch, dropRoutes);
  }

  /** The person said no: nothing is sent. */
  cancel(): void {
    if (this.current.kind !== "confirm") return;
    this.pending = null;
    this.move({ kind: "idle" });
  }

  private ask(patch: JsonObject, words: string, routes: RouteEnds[]): void {
    this.pending = { patch, dropRoutes: routes.length > 0 };
    this.move({ kind: "confirm", words, routes, dropRoutes: routes.length > 0 });
  }

  private async send(patch: JsonObject, dropRoutes: boolean): Promise<void> {
    this.pending = null;
    this.move({ kind: "sending" });
    try {
      const params: Methods["node.set"]["params"] = { name: this.node, settings: patch };
      if (dropRoutes) params.dropRoutes = true;
      const result = await this.client.set(params);
      this.move({ kind: "applied", applied: result.applied });
    } catch (error) {
      const data = error instanceof RpcError ? error.data : undefined;
      if (data?.code === "ports-in-use" && data.routes !== undefined) {
        const routes = data.routes;
        return this.ask(patch, confirmWords(this.node, { applied: "reshape", routes }), routes);
      }
      this.move(refusedStep(error));
    }
  }

  private move(step: ApplyStep): void {
    this.current = step;
    this.onStep(step);
  }
}
