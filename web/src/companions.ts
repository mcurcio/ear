// A node's companions in words and badges (docs/editor.md#companions): the one place that turns the wire's companion
// list (docs/control-port.md#companions) into what the box's strip and the node panel draw. A kind this editor does
// not know is drawn by its own text; dropped and shortfall are totals in words, and a badge's warning mark shows only
// while its counter rose in the last minute on the browser's monotonic clock (performance.now, so a step of the wall
// clock never stretches or cuts the minute), the one rule of marks.ts, which the counters' marks follow too.
import type { GraphState } from "./graph";
import { GraphMoves, isRecent, soonestExpiry, type HeldValues } from "./marks";
import type { Companion, CompanionKind, GraphNode } from "./protocol";

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

/** Each node's companions' counters, by node and then by companion; an engine kind has none. */
function companionCounters(graph: GraphState): HeldValues {
  const values: HeldValues = new Map();
  for (const node of graph?.nodes ?? []) {
    const counters = new Map<string, number>();
    for (const companion of node.companions ?? []) {
      const counter = counterOf(companion);
      if (counter !== undefined) counters.set(companion.name, counter.value);
    }
    if (counters.size > 0) values.set(node.name, counters);
  }
  return values;
}

/**
 * Each companion's last counter value and when it last rose, per node, by the mark's one rule (marks.ts): a first
 * sighting, a fall (a new configuration) and an entry seen again after its list dropped it are not rises, and a closed
 * connection forgets everything held.
 */
export class CompanionMoves extends GraphMoves {
  constructor() {
    super(companionCounters);
  }

  /** When the node's next shown mark leaves (the box redraws then), or undefined when none shows. */
  nextExpiry(node: GraphNode, now: number): number | undefined {
    return soonestExpiry(
      drawnList(node).map((companion) => this.movedAt(node.name, companion.name)),
      now,
    );
  }
}

/** The badges of a node's strip, in list order; [] for a node with none, or failed or disabled. */
export function companionBadges(node: GraphNode, moves: CompanionMoves, now: number): CompanionBadge[] {
  return drawnList(node).map((companion) => {
    const counter = counterOf(companion);
    const moved = moves.movedAt(node.name, companion.name);
    const warning = counter !== undefined && isRecent(moved, now);
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
