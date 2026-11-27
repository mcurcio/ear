// What a live control may do and shows, pure (docs/editor.md#live-numbers-faders-and-knobs): when the editor's live
// controls are disabled and why, which open gesture of another editor holds a key, the value a control shows, and
// the knobs a node's box draws. Nothing here names a node type: the schema says which rows are live.
import { isFaderRow } from "./form";
import type { GraphFile, GraphNode, NodeGesture, SettingSchema, TypeSchema } from "./protocol";
import type { ConnectionStatus } from "./rpc";
import { travelOf, valueAfterKey, valueAt, type TaperRow } from "./taper";

/**
 * Why live controls are disabled now, or undefined: an edit would be refused (the graph file has errors, or the graph
 * refused it, so nothing could commit), or nothing would reach ear (the connection is not open).
 */
export function liveDisabledReason(connection: ConnectionStatus, file: GraphFile | null | undefined): string | undefined {
  if (connection !== "open") return "not connected to ear";
  if (file === null || file === undefined) return undefined;
  if (file.diagnostics.some((diagnostic) => diagnostic.severity === "error")) return "ear.conf has errors";
  if (file.refusal !== undefined) return "the graph refused ear.conf";
  return undefined;
}

/** The open gesture of another editor (or another tab) holding `key` of `node`: one whose id this editor does not hold. */
export function heldByOther(node: GraphNode, key: string, ownId: string | undefined): NodeGesture | undefined {
  return node.gestures?.find((held) => held.key === key && held.gesture !== ownId);
}

/** The node's value of `row`'s key as a number: its setting, else the row's default, else its min. */
export function nodeNumber(node: GraphNode, row: SettingSchema): number {
  const value = node.settings[row.key];
  if (typeof value === "number") return value;
  if (typeof row.default === "number") return row.default;
  return row.min ?? 0;
}

/**
 * The value a live control shows: its own gesture's while it holds one; else another editor's hand, while one holds
 * it; else the node's. A control follows ear except while the person holds it.
 */
export function shownNumber(
  node: GraphNode,
  row: SettingSchema,
  own: { value?: number; id?: string; state: string },
): number {
  if (own.state !== "idle" && own.value !== undefined) return own.value;
  const held = heldByOther(node, row.key, own.id);
  if (held !== undefined) return held.value;
  return nodeNumber(node, row);
}

/** The knobs a box draws: each live ramped decimal not in a family, when the type has one or two of them. */
export function boxKnobRows(schema: TypeSchema | undefined): SettingSchema[] {
  if (schema === undefined) return [];
  const rows = schema.settings.filter((row) => row.indexed === undefined && isFaderRow(row));
  return rows.length >= 1 && rows.length <= 2 ? rows : [];
}

/** The hidden range input's scale: travel in thousandths; a screen reader's swipe moves it a step, 1% of the travel. */
export const inputScale = 1000;
export const inputStep = 10;

/** The hidden range input's value for a control showing `value`. */
export function inputValueOf(row: TaperRow, value: number): number {
  return Math.round(travelOf(row, value) * inputScale);
}

/** The value the hidden range input's own value stands for (a screen reader's change). */
export function valueOfInput(row: TaperRow, input: string): number {
  const travel = Number(input) / inputScale;
  return valueAt(row, Number.isFinite(travel) ? travel : 0);
}

/**
 * What a key on a focused live control does (docs/editor.md#faders-and-knobs): Escape cancels a held gesture; an arrow,
 * Page Up or Down, Home or End moves it as a gesture (nudge); any other key is not the control's (and, inside .nokey,
 * no shortcut's either: Backspace and `m` send nothing).
 */
export function rangeKey(
  row: TaperRow,
  value: number,
  key: string,
  shift: boolean,
  holding: boolean,
): { escape: true } | { nudge: number } | undefined {
  if (key === "Escape") return holding ? { escape: true } : undefined;
  const next = valueAfterKey(row, value, key, shift);
  return next !== undefined ? { nudge: next } : undefined;
}
