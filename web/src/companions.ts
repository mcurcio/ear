// A node's companions in words and badges (docs/editor.md#companions): the one place that turns the wire's companion
// list (docs/control-port.md#companions) into what the box's strip and the node panel draw. A kind this editor does
// not know is drawn by its own text; dropped and shortfall are totals in words, and a badge's warning mark shows only
// while its counter rose in the last minute on the browser's clock, the rule the counters' marks follow too.
import type { GraphState, GraphStore } from "./graph";
import type { Companion, CompanionKind, GraphNode } from "./protocol";

/** How long a counter's rise keeps its badge's warning mark. */
export const recentMarkMs = 60_000;

export interface CompanionBadge {
  /** The companion's name. */
  key: string;
  /** Its kind as a short word, or an unknown kind's own text. */
  label: string;
  ready: boolean;
  readyText: "ready" | "not ready";
  /** Its dropped or shortfall rose in the last minute. */
  warning: boolean;
  /** The mark's words ("" without the mark). */
  warningText: string;
  /** companionDetail, with the mark's words when it shows. */
  title: string;
}

const labels: Record<string, string> = {
  "card-playback": "card out",
  "card-capture": "card in",
  "rtp-receiver": "rtp",
  sink: "sink",
  source: "source",
};

export function companionLabel(kind: CompanionKind): string {
  return Object.hasOwn(labels, kind) ? labels[kind]! : kind;
}

/** The counter an entry carries, whatever its kind: a sink's dropped, a source's shortfall; none on an engine kind. */
function counterOf(companion: Companion): { value: number; words: string; recent: string } | undefined {
  if (typeof companion.dropped === "number")
    return {
      value: companion.dropped,
      words: "dropped since it was made",
      recent: "dropped frames in the last minute",
    };
  if (typeof companion.shortfall === "number")
    return {
      value: companion.shortfall,
      words: "played short since it was made",
      recent: "played short in the last minute",
    };
  return undefined;
}

/** "rec.writer (sink on writer): ready; 4096 frames dropped since it was made". */
export function companionDetail(companion: Companion): string {
  const head = `${companion.name} (${companionLabel(companion.kind)} on ${companion.bank}): ${
    companion.ready ? "ready" : "not ready"
  }`;
  const counter = counterOf(companion);
  if (counter === undefined) return head;
  return `${head}; ${counter.value} ${counter.value === 1 ? "frame" : "frames"} ${counter.words}`;
}

/** The list a node's box draws: none for a failed or disabled node (its configuration was torn down). */
function drawnList(node: GraphNode): Companion[] {
  if (node.phase === "failed" || node.phase === "disabled") return [];
  return node.companions ?? [];
}

interface Held {
  value: number;
  movedAt?: number;
}

/**
 * Each companion's last counter value and when it last rose, per node, on the browser's clock. A rise is a value
 * above the one held for the same node and companion; a first sighting, a fall (a new configuration) and an entry
 * seen again after its list dropped it are not. Kept outside the reducer, which holds only what ear sent.
 */
export class CompanionMoves {
  private held = new Map<string, Map<string, Held>>();

  /** Takes the graph as it now is, at `now` (ms on the browser's clock). */
  note(graph: GraphState, now: number): void {
    const next = new Map<string, Map<string, Held>>();
    for (const node of graph?.nodes ?? []) {
      const before = this.held.get(node.name);
      const entries = new Map<string, Held>();
      for (const companion of node.companions ?? []) {
        const counter = counterOf(companion);
        if (counter === undefined) continue;
        const old = before?.get(companion.name);
        if (old === undefined || counter.value < old.value) entries.set(companion.name, { value: counter.value });
        else if (counter.value > old.value) entries.set(companion.name, { value: counter.value, movedAt: now });
        else entries.set(companion.name, old);
      }
      if (entries.size > 0) next.set(node.name, entries);
    }
    this.held = next;
  }

  /** When the companion's counter last rose, or undefined if it has not since it was first seen. */
  movedAt(node: string, companion: string): number | undefined {
    return this.held.get(node)?.get(companion)?.movedAt;
  }

  /** When the node's next shown mark leaves (the box redraws then), or undefined when none shows. */
  nextExpiry(node: GraphNode, now: number): number | undefined {
    let next: number | undefined;
    for (const companion of drawnList(node)) {
      const moved = this.movedAt(node.name, companion.name);
      if (moved === undefined) continue;
      const leaves = moved + recentMarkMs;
      if (leaves > now && (next === undefined || leaves < next)) next = leaves;
    }
    return next;
  }

  /**
   * Notes every graph the store makes from now on. Attach before anything that draws subscribes, so a box drawn for a
   * new graph reads the moves of that graph. Returns the unsubscribe.
   */
  attach(store: GraphStore, clock: () => number = Date.now): () => void {
    this.note(store.get(), clock());
    return store.subscribe(() => this.note(store.get(), clock()));
  }
}

/** The badges of a node's strip, in list order; [] for a node with none, or failed or disabled. */
export function companionBadges(node: GraphNode, moves: CompanionMoves, now: number): CompanionBadge[] {
  return drawnList(node).map((companion) => {
    const counter = counterOf(companion);
    const moved = moves.movedAt(node.name, companion.name);
    const warning = counter !== undefined && moved !== undefined && now - moved < recentMarkMs;
    const detail = companionDetail(companion);
    const warningText = warning && counter !== undefined ? counter.recent : "";
    return {
      key: companion.name,
      label: companionLabel(companion.kind),
      ready: companion.ready,
      readyText: companion.ready ? "ready" : "not ready",
      warning,
      warningText,
      title: warning ? `${detail} (${warningText})` : detail,
    };
  });
}
