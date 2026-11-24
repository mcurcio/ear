// The canvas's browser-side state (docs/editor.md#the-canvas): the flow nodes and their places, the wires as ear
// announced them, the selection the inspector follows (docs/editor.md#the-inspector), and Tidy. Moves and selection
// are the browser's; adding and removing wait for ear's events (docs/editor.md#its-rules).
import { applyNodeChanges, type EdgeChange, type NodeChange } from "@xyflow/react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { edgeId, handleId } from "../connect";
import type { GraphState } from "../graph";
import { Places, type Position } from "../layout";
import { selectionKey, selectionOf, withSelected, type Selection } from "../selection";
import type { EarFlowNode } from "./EarNode";
import type { LevelFlowEdge } from "./LevelEdge";

/** How long Tidy's slide lasts: theme.css's --motion-slow (0 under reduced motion, where the CSS does not slide). */
const tidyMs = 180;

export interface CanvasModel {
  nodes: EarFlowNode[];
  edges: LevelFlowEdge[];
  onNodesChange: (changes: NodeChange<EarFlowNode>[]) => void;
  onEdgesChange: (changes: EdgeChange<LevelFlowEdge>[]) => void;
  /** Its identity changes only with the selection. */
  selection: Selection;
  select: (selection: Selection) => void;
  /** Every node laid out again (layout.ts's relayout); nothing is sent. */
  tidy: () => void;
  /** While Tidy's nodes slide to their new places. */
  tidying: boolean;
}

/**
 * The canvas's nodes for a new graph: kept nodes keep their place, a node replaced under the same
 * name takes its old place, new ones are laid out.
 */
function syncNodes(previous: EarFlowNode[], graph: GraphState, places: Places): EarFlowNode[] {
  if (graph === null) return [];
  const kept = new Map(previous.map((node) => [node.id, node]));
  const current = new Map<string, Position>(previous.map((node) => [node.id, node.position]));
  const positions = places.place(graph, current);
  return graph.nodes.map((node) => {
    const old = kept.get(node.name);
    if (old !== undefined) return { ...old, data: { node } };
    return { id: node.name, type: "ear", position: positions.get(node.name)!, data: { node } };
  });
}

export function useCanvas(graph: GraphState): CanvasModel {
  const [nodes, setNodes] = useState<EarFlowNode[]>([]);
  const [selectedEdges, setSelectedEdges] = useState<ReadonlySet<string>>(new Set());
  const [places] = useState(() => new Places());
  const [tidying, setTidying] = useState(false);
  const tidyTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);

  useEffect(() => setNodes((previous) => syncNodes(previous, graph, places)), [graph, places]);
  useEffect(() => () => clearTimeout(tidyTimer.current), []);

  const edges = useMemo<LevelFlowEdge[]>(
    () =>
      (graph?.routes ?? []).map((route) => {
        const id = edgeId(route);
        return {
          id,
          type: "level",
          source: route.from.node,
          sourceHandle: handleId("out", route.from.port),
          target: route.to.node,
          targetHandle: handleId("in", route.to.port),
          selected: selectedEdges.has(id),
          data: { route },
        };
      }),
    [graph, selectedEdges],
  );

  const onNodesChange = useCallback(
    (changes: NodeChange<EarFlowNode>[]) =>
      setNodes((current) => applyNodeChanges(changes.filter((c) => c.type !== "remove" && c.type !== "add"), current)),
    [],
  );
  const onEdgesChange = useCallback((changes: EdgeChange<LevelFlowEdge>[]) => {
    setSelectedEdges((current) => {
      const next = new Set(current);
      for (const change of changes) {
        if (change.type !== "select") continue;
        if (change.selected) next.add(change.id);
        else next.delete(change.id);
      }
      return next;
    });
  }, []);

  const current = selectionOf(nodes, selectedEdges);
  const key = selectionKey(current);
  // Memoised on the key alone: the selection's identity changes only when what is selected does, not on every move.
  const selection = useMemo(() => current, [key]);

  const select = useCallback((next: Selection) => {
    setNodes((existing) => withSelected(existing, new Set(next.nodes)));
    setSelectedEdges(new Set(next.routes));
  }, []);

  const graphRef = useRef(graph);
  graphRef.current = graph;
  const tidy = useCallback(() => {
    const shown = graphRef.current;
    if (shown === null) return;
    const positions = places.relayout(shown);
    setNodes((existing) => existing.map((node) => ({ ...node, position: positions.get(node.id) ?? node.position })));
    setTidying(true);
    clearTimeout(tidyTimer.current);
    tidyTimer.current = setTimeout(() => setTidying(false), tidyMs);
  }, [places]);

  return useMemo(
    () => ({ nodes, edges, onNodesChange, onEdgesChange, selection, select, tidy, tidying }),
    [nodes, edges, onNodesChange, onEdgesChange, selection, select, tidy, tidying],
  );
}
