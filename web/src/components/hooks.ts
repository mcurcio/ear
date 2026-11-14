// React's view of the editor session: the graph, the graph file, the connection and one port's level, each read
// from its store with useSyncExternalStore, so a levels message re-renders only the meters and
// wires of the ports that changed; and a node's companion badges at the browser's time.
import { createContext, useCallback, useContext, useEffect, useState, useSyncExternalStore } from "react";
import { companionBadges, type CompanionBadge } from "../companions";
import type { Editor } from "../editor";
import type { GraphState } from "../graph";
import type { GraphFileState } from "../graphFile";
import type { GraphNode, Level } from "../protocol";
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
 * The node's companion badges (companions.ts) at the browser's time, drawn again when a shown warning mark's minute
 * ends; a new graph (a new node object) draws them again anyway.
 */
export function useCompanionBadges(node: GraphNode): CompanionBadge[] {
  const { moves } = useEditor();
  const [, redraw] = useState(0);
  const now = performance.now();
  const leaves = moves.nextExpiry(node, now);
  useEffect(() => {
    if (leaves === undefined) return undefined;
    const timer = setTimeout(() => redraw((n) => n + 1), Math.max(0, leaves - performance.now()));
    return () => clearTimeout(timer);
  }, [leaves]);
  return companionBadges(node, moves, now);
}
