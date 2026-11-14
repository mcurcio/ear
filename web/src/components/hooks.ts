// React's view of the editor session: the graph, the graph file, the connection and one port's level, each read
// from its store with useSyncExternalStore, so a levels message re-renders only the meters and
// wires of the ports that changed; and a node's companion badges and the counters' marks at the browser's time.
import { createContext, useCallback, useContext, useEffect, useState, useSyncExternalStore } from "react";
import { companionBadges, type CompanionBadge } from "../companions";
import {
  dropoutBadge,
  graphCounterRows,
  nodeCounterMark,
  nodeCounterRows,
  type CounterMark,
  type CounterRow,
  type DropoutBadge,
} from "../counters";
import type { Editor } from "../editor";
import type { GraphState } from "../graph";
import type { GraphFileState } from "../graphFile";
import type { Graph, GraphNode, Level } from "../protocol";
import type { ConnectionStatus } from "../rpc";

export const EditorContext = createContext<Editor | null>(null);

export function useEditor(): Editor {
  const editor = useContext(EditorContext);
  if (editor === null) throw new Error("useEditor outside EditorContext");
  return editor;
}

export function useGraph(): GraphState {
  const { graph } = useEditor();
  return useSyncExternalStore(
    useCallback((listener) => graph.subscribe(listener), [graph]),
    () => graph.get(),
  );
}

export function useGraphFile(): GraphFileState | null {
  const { file } = useEditor();
  return useSyncExternalStore(
    useCallback((listener) => file.subscribe(listener), [file]),
    () => file.get(),
  );
}

export function useConnection(): ConnectionStatus {
  const editor = useEditor();
  return useSyncExternalStore(
    useCallback((listener) => editor.subscribeConnection(listener), [editor]),
    () => editor.connection(),
  );
}

export function useLevel(node: string, port: string): Level {
  const { levels } = useEditor();
  return useSyncExternalStore(
    useCallback((listener) => levels.subscribe(node, port, listener), [levels, node, port]),
    () => levels.get(node, port),
  );
}

/**
 * The browser's monotonic time now, drawn again at `nextChange(now)`: when a shown mark's minute ends or a count of the
 * last minute falls. A new graph (a new node object) draws again anyway.
 */
function useRecentNow(nextChange: (now: number) => number | undefined): number {
  const [, redraw] = useState(0);
  const now = performance.now();
  const leaves = nextChange(now);
  useEffect(() => {
    if (leaves === undefined) return undefined;
    const timer = setTimeout(() => redraw((n) => n + 1), Math.max(0, leaves - performance.now()));
    return () => clearTimeout(timer);
  }, [leaves]);
  return now;
}

/** The node's companion badges (companions.ts) at the browser's time. */
export function useCompanionBadges(node: GraphNode): CompanionBadge[] {
  const { moves } = useEditor();
  const now = useRecentNow((t) => moves.nextExpiry(node, t));
  return companionBadges(node, moves, now);
}

/** The node's counters (counters.ts) at the browser's time: its box's mark and its panel's rows. */
export function useNodeCounters(node: GraphNode): { mark: CounterMark; rows: CounterRow[] } {
  const { counterMoves } = useEditor();
  const now = useRecentNow((t) => counterMoves.nextNodeChange(node, t));
  return { mark: nodeCounterMark(node, counterMoves, now), rows: nodeCounterRows(node, counterMoves, now) };
}

/** The graph's counters (counters.ts) at the browser's time: the dropouts badge and the panel's rows. */
export function useGraphCounters(graph: Graph): { badge: DropoutBadge | undefined; rows: CounterRow[] } {
  const { counterMoves } = useEditor();
  const now = useRecentNow((t) => counterMoves.nextGraphChange(t));
  return { badge: dropoutBadge(graph, counterMoves, now), rows: graphCounterRows(graph, counterMoves, now) };
}
