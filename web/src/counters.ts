// The diagnostics counters in words and marks (docs/editor.md#counters): the one place that turns the wire's counters
// (docs/control-port.md#counters) into the panel's totals, a node box's warning mark and the graph's dropouts badge.
// The panel always holds every total, an unknown counter by its name and number; the mark and the badge show only
// while a loss counter rose in the last minute, by the one rule companions' badges follow (marks.ts). `cycles` moves
// every cycle and is never a loss; an unknown counter may count anything, so it is shown and never marked.
import type { GraphState } from "./graph";
import { GraphMoves, type HeldValues } from "./marks";
import type { Counters, Graph, GraphNode } from "./protocol";

interface Known {
  key: string;
  /** The panel's label. */
  label: string;
  /** What it counts, the row's tooltip. */
  means: string;
  /** "missed 12 cycles": the count in words, for the mark's and the badge's tooltips. */
  phrase?: (n: number) => string;
  /** The value with its unit. */
  text?: (n: number) => string;
  /** A rise is a loss, which marks; false for a total that moves on its own or follows another. */
  loss: boolean;
}

const plural = (n: number, one: string, many: string) => `${n} ${n === 1 ? one : many}`;

/** The graph's known counters, in the panel's order. */
const graphKnown: Known[] = [
  { key: "cycles", label: "cycles", means: "cycles the internal clock started since ear started", loss: false },
  {
    key: "cutCycles",
    label: "dropouts",
    means: "cycles the driver cut short: a dropout on every output",
    phrase: (n) => plural(n, "dropout", "dropouts"),
    loss: true,
  },
  {
    key: "stalls",
    label: "stalls",
    means: "times the graph fell behind and lost cycles, one each however many it cut",
    phrase: (n) => plural(n, "stall", "stalls"),
    loss: true,
  },
  // Follows cutCycles (each cut is one block), so it marks nothing of its own.
  { key: "lostMs", label: "time lost", means: "the time the cut cycles lost", text: (n) => `${n} ms`, loss: false },
];

/** A node's known counters, in the panel's order: every one a loss (docs/control-port.md#counters). */
const nodeKnown: Known[] = [
  {
    key: "missedCycles",
    label: "missed cycles",
    means: "cycles its block missed that the driver did not cut: it was not scheduled",
    phrase: (n) => `missed ${plural(n, "cycle", "cycles")}`,
    loss: true,
  },
  {
    key: "overruns",
    label: "overruns",
    means: "cycles in which its own work took longer than the cycle",
    phrase: (n) => `overran ${plural(n, "time", "times")}`,
    loss: true,
  },
  {
    key: "repositions",
    label: "repositions",
    means: "cycles in which its position moved by what no missed cycle explains",
    phrase: (n) => `repositioned ${plural(n, "time", "times")}`,
    loss: true,
  },
  {
    key: "driverChanges",
    label: "driver changes",
    means: "cycles in which its driver changed",
    phrase: (n) => `changed driver ${plural(n, "time", "times")}`,
    loss: true,
  },
];

const graphGroup = "graph";
const nodeGroupPrefix = "node ";
const nodeGroup = (name: string) => `${nodeGroupPrefix}${name}`;

function knownOf(list: Known[], key: string): Known | undefined {
  return list.find((known) => known.key === key);
}

/** The loss counters a mark watches, by the mark's group. */
function lossCounters(graph: GraphState): HeldValues {
  const values: HeldValues = new Map();
  const watch = (group: string, counters: Counters | undefined, list: Known[]) => {
    const held = new Map<string, number>();
    for (const known of list)
      if (known.loss && typeof counters?.[known.key] === "number") held.set(known.key, counters[known.key]!);
    if (held.size > 0) values.set(group, held);
  };
  watch(graphGroup, graph?.counters, graphKnown);
  for (const node of graph?.nodes ?? []) watch(nodeGroup(node.name), node.counters, nodeKnown);
  return values;
}

/**
 * The loss counters' last values and their rises, the graph's and each node's, by the mark's one rule (marks.ts): a
 * first sighting (a snapshot's, a new node's), a fall and a counter seen again after the graph dropped it are no rise,
 * and a closed connection forgets everything held. A removed node's values are kept, so a port-caused replace's
 * carried counts continue its mark and a new node under the name (its zeros a fall) starts fresh.
 */
export class CounterMoves extends GraphMoves {
  constructor() {
    // A node's counters never step back while it lives, and only a port-caused replace's node.added carries any:
    // keeping a removed node's values lets that replace's mark go on, and a new node's zeros are a fall.
    super(lossCounters, (group) => group.startsWith(nodeGroupPrefix));
  }

  /** How much the node's counter rose in the last minute. */
  nodeRisenBy(node: string, key: string, now: number): number {
    return this.risenBy(nodeGroup(node), key, now);
  }

  /** How much the graph's counter rose in the last minute. */
  graphRisenBy(key: string, now: number): number {
    return this.risenBy(graphGroup, key, now);
  }

  /** When the node's mark or a count of its rows next changes (its box and panel redraw then); undefined with none. */
  nextNodeChange(node: GraphNode, now: number): number | undefined {
    return earliest(nodeKnown.map((known) => this.nextLeave(nodeGroup(node.name), known.key, now)));
  }

  /** When the dropouts badge or a count of the graph's rows next changes; undefined with none. */
  nextGraphChange(now: number): number | undefined {
    return earliest(graphKnown.map((known) => this.nextLeave(graphGroup, known.key, now)));
  }
}

function earliest(times: (number | undefined)[]): number | undefined {
  const known = times.filter((t): t is number => t !== undefined);
  return known.length === 0 ? undefined : Math.min(...known);
}

export interface CounterRow {
  key: string;
  label: string;
  /** The value as shown, with its unit. */
  text: string;
  /** What it counts; a counter this editor does not know says so. */
  title: string;
  /** It rose in the last minute: the row carries the mark. */
  warning: boolean;
  /** The mark's words ("" without the mark). */
  warningText: string;
  known: boolean;
}

/** The counters' names: the known in the list's order, then the unknown as sent. */
function orderedKeys(counters: Counters, list: Known[]): string[] {
  const known = list.filter((k) => Object.hasOwn(counters, k.key)).map((k) => k.key);
  return [...known, ...Object.keys(counters).filter((key) => knownOf(list, key) === undefined)];
}

function rowsOf(
  counters: Counters | undefined,
  list: Known[],
  risen: (key: string) => number,
): CounterRow[] {
  if (counters === undefined) return [];
  const row = (key: string, known: Known | undefined): CounterRow => {
    const value = counters[key];
    const shown = typeof value === "number" && known?.text !== undefined ? known.text(value) : String(value);
    // Only the loss counters are held (lossCounters), so any other rises by nothing.
    const by = risen(key);
    return {
      key,
      label: known?.label ?? key,
      text: shown,
      title: known?.means ?? "a counter this editor does not know",
      warning: by > 0,
      warningText: by > 0 ? `${by} more in the last minute` : "",
      known: known !== undefined,
    };
  };
  return orderedKeys(counters, list).map((key) => row(key, knownOf(list, key)));
}

/** The panel's rows of the graph's counters, known in order and then unknown as sent; [] without counters. */
export function graphCounterRows(graph: Graph, moves: CounterMoves, now: number): CounterRow[] {
  return rowsOf(graph.counters, graphKnown, (key) => moves.graphRisenBy(key, now));
}

/** The node panel's rows of its counters, known in order and then unknown as sent; [] without counters. */
export function nodeCounterRows(node: GraphNode, moves: CounterMoves, now: number): CounterRow[] {
  return rowsOf(node.counters, nodeKnown, (key) => moves.nodeRisenBy(node.name, key, now));
}

/** The counts in words, in the panel's order: "missed 12 cycles, overran 3 times". */
function phrases(counters: Counters, list: Known[], value: (key: string) => number, only: (key: string) => boolean): string {
  return orderedKeys(counters, list)
    .filter((key) => only(key) && value(key) > 0)
    .map((key) => knownOf(list, key)?.phrase?.(value(key)) ?? `${key} ${value(key)}`)
    .join(", ");
}

export interface CounterMark {
  /** A loss counter of the node's rose in the last minute. */
  warning: boolean;
  /** The mark's words for a screen reader ("" without the mark). */
  warningText: string;
  /** The tooltip: the last minute's and the totals in words ("" without the mark). */
  title: string;
}

const noMark: CounterMark = { warning: false, warningText: "", title: "" };

/** The node box's mark beside its state word: shown while a loss counter of its own rose in the last minute. */
export function nodeCounterMark(node: GraphNode, moves: CounterMoves, now: number): CounterMark {
  const counters = node.counters;
  if (counters === undefined) return noMark;
  // What rose is what the moves hold, the loss counters (lossCounters); the totals name the same counters.
  const recent = phrases(counters, nodeKnown, (key) => moves.nodeRisenBy(node.name, key, now), () => true);
  if (recent === "") return noMark;
  const isLoss = (key: string) => knownOf(nodeKnown, key)?.loss === true;
  const totals = phrases(counters, nodeKnown, (key) => (typeof counters[key] === "number" ? counters[key]! : 0), isLoss);
  const warningText = `in the last minute: ${recent}`;
  return { warning: true, warningText, title: `In the last minute: ${recent}. Since it was added: ${totals}.` };
}

export interface DropoutBadge {
  /** Cycles cut in the last minute. */
  count: number;
  /** "3 dropouts". */
  text: string;
  /** The last minute's and the totals in words. */
  title: string;
}

/** The graph's dropouts badge: shown while cutCycles rose in the last minute, with how much; else undefined. */
export function dropoutBadge(graph: Graph, moves: CounterMoves, now: number): DropoutBadge | undefined {
  const count = moves.graphRisenBy("cutCycles", now);
  const counters = graph.counters;
  if (count === 0 || counters === undefined) return undefined;
  const text = plural(count, "dropout", "dropouts");
  let since = plural(counters.cutCycles, "dropout", "dropouts");
  if (typeof counters.lostMs === "number") since += ` (${counters.lostMs} ms)`;
  if (typeof counters.stalls === "number") since += ` in ${plural(counters.stalls, "stall", "stalls")}`;
  return {
    count,
    text,
    title: `The host held up the audio thread: ${text} in the last minute; ${since} since ear started.`,
  };
}
