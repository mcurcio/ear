// The warning mark's one rule (docs/editor.md#recent-marks): which counters rose in the last minute, on the browser's
// monotonic clock (performance.now, so a step of the wall clock never stretches or cuts the minute). Companions' badges
// (companions.ts) and the diagnostics counters' marks (counters.ts) both keep their counters' values here, each with
// its own reading of the graph, so the two marks can never follow different rules.
import type { GraphState, GraphStore } from "./graph";

/** How long a counter's rise keeps its mark. */
export const recentMarkMs = 60_000;

/** Each counter's value now, by group (a node, the graph) and then by counter. */
export type HeldValues = Map<string, Map<string, number>>;

interface Rise {
  at: number;
  by: number;
}

interface Held {
  value: number;
  /** When it last rose. */
  movedAt?: number;
  /** Its rises of the last minute, oldest first. */
  rises: Rise[];
}

/** True while a rise at `movedAt` is still in its minute at `now`. */
export function isRecent(movedAt: number | undefined, now: number): boolean {
  return movedAt !== undefined && now - movedAt < recentMarkMs;
}

/** When the soonest of these rises leaves its minute, after `now`; undefined when none shows. */
export function soonestExpiry(movedAts: Iterable<number | undefined>, now: number): number | undefined {
  let next: number | undefined;
  for (const moved of movedAts) {
    if (moved === undefined) continue;
    const leaves = moved + recentMarkMs;
    if (leaves > now && (next === undefined || leaves < next)) next = leaves;
  }
  return next;
}

/**
 * Each counter's last value and its rises, per group, on the browser's monotonic clock. A rise is a value above the one
 * held for the same group and counter; a first sighting, a fall (a new configuration, a new node) and a counter seen
 * again after the graph dropped it (its group or the counter gone from what `valuesOf` read) are not. Everything held
 * is forgotten when the connection closes, so a reconnect's snapshot is a first sighting. Kept outside the reducer,
 * which holds only what ear sent.
 */
export class GraphMoves {
  private held = new Map<string, Map<string, Held>>();

  /** `valuesOf` reads the counters this mark watches from a graph. */
  constructor(private readonly valuesOf: (graph: GraphState) => HeldValues) {}

  /** Forgets every value held: what moved while the editor was away did not move in its last minute. */
  forget(): void {
    this.held = new Map();
  }

  /** Takes the graph as it now is, at `now` (ms on the browser's monotonic clock). */
  note(graph: GraphState, now: number): void {
    const next = new Map<string, Map<string, Held>>();
    for (const [group, values] of this.valuesOf(graph)) {
      const before = this.held.get(group);
      const entries = new Map<string, Held>();
      for (const [key, value] of values) {
        const old = before?.get(key);
        const rises = old?.rises.filter((rise) => isRecent(rise.at, now)) ?? [];
        if (old === undefined || value < old.value) entries.set(key, { value, rises: [] });
        else if (value > old.value)
          entries.set(key, { value, movedAt: now, rises: [...rises, { at: now, by: value - old.value }] });
        else entries.set(key, { ...old, rises });
      }
      if (entries.size > 0) next.set(group, entries);
    }
    this.held = next;
  }

  /** When the counter last rose, or undefined if it has not since it was first seen. */
  movedAt(group: string, key: string): number | undefined {
    return this.held.get(group)?.get(key)?.movedAt;
  }

  /** How much the counter rose in the minute before `now`: 0 when it did not. */
  risenBy(group: string, key: string, now: number): number {
    let sum = 0;
    for (const rise of this.held.get(group)?.get(key)?.rises ?? []) if (isRecent(rise.at, now)) sum += rise.by;
    return sum;
  }

  /** When the counter's oldest rise of the last minute leaves it (risenBy falls then), or undefined with none. */
  nextLeave(group: string, key: string, now: number): number | undefined {
    return soonestExpiry(
      (this.held.get(group)?.get(key)?.rises ?? []).map((rise) => rise.at),
      now,
    );
  }

  /**
   * Notes every graph the store makes from now on. Attach before anything that draws subscribes, so a box drawn for a
   * new graph reads the moves of that graph. Returns the unsubscribe.
   */
  attach(store: GraphStore, clock: () => number = () => performance.now()): () => void {
    this.note(store.get(), clock());
    return store.subscribe(() => this.note(store.get(), clock()));
  }
}
