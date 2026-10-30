// React's view of the editor session: the graph, the connection and one port's level, each read
// from its store with useSyncExternalStore, so a levels message re-renders only the meters and
// wires of the ports that changed.
import { createContext, useCallback, useContext, useSyncExternalStore } from "react";
import type { Editor } from "../editor";
import type { GraphState } from "../graph";
import type { Level } from "../protocol";
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
