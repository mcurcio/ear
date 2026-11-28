// The canvas's browser-side state (docs/editor.md#the-canvas): the flow nodes and their places, the wires as ear
// announced them, the selection the inspector follows (docs/editor.md#the-inspector), and Tidy. Moves and selection
// are the browser's; adding and removing wait for ear's events (docs/editor.md#its-rules). Beside the routes, the ghost
// wires of route requests on their way (requests.ts, docs/editor.md#requests-in-flight): requests, never routes, so
// they cannot be selected, focused or deleted. Places are kept per ear in the browser through the keeper App gives
// (prefs.ts, docs/editor.md#what-the-browser-keeps): a drag's end, new nodes' places and Tidy are saved.
import { applyNodeChanges, type EdgeChange, type NodeChange } from "@xyflow/react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { edgeId, handleId } from "../connect";
import type { GraphState } from "../graph";
import { Places, type PlaceHint, type PlaceKeeper, type Position } from "../layout";
import { ghostWires } from "../requests";
import { selectionKey, selectionOf, withSelected, type Selection } from "../selection";
import type { EarFlowNode } from "./EarNode";
import type { GhostFlowEdge } from "./GhostEdge";
import { useEditor, useExpiry, useRequests } from "./hooks";
import type { LevelFlowEdge } from "./LevelEdge";

/** How long Tidy's slide lasts: patchbay's --pb-motion-slow (0 under reduced motion, where the CSS does not slide). */
const tidyMs = 180;

/** A route's wire, or a route request's ghost. */
export type CanvasEdge = LevelFlowEdge | GhostFlowEdge;

export interface CanvasModel {
  nodes: EarFlowNode[];
  edges: CanvasEdge[];
  onNodesChange: (changes: NodeChange<EarFlowNode>[]) => void;
  onEdgesChange: (changes: EdgeChange<CanvasEdge>[]) => void;
  /** Its identity changes only with the selection. */
  selection: Selection;
  select: (selection: Selection) => void;
  /** Every node laid out again (layout.ts's relayout); nothing is sent. */
  tidy: () => void;
  /** While Tidy's nodes slide to their new places. */
  tidying: boolean;
  /** Quick add's placement hint (docs/editor.md#adding-on-the-canvas); null drops it. */
  hint: (hint: PlaceHint | null) => void;
  /** The hint held until a node arriving takes it or drops it: where an add on its way draws its ghost. */
  hinted: PlaceHint | null;
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

export function useCanvas(graph: GraphState, keeper?: PlaceKeeper): CanvasModel {
  const [nodes, setNodes] = useState<EarFlowNode[]>([]);
  const [selectedEdges, setSelectedEdges] = useState<ReadonlySet<string>>(new Set());
  const [places] = useState(() => new Places(keeper));
  const [tidying, setTidying] = useState(false);
  const [hinted, setHinted] = useState<PlaceHint | null>(null);
  const tidyTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const { requests } = useEditor();
  const asked = useRequests();
  const expireGhosts = useCallback(() => requests.expire(), [requests]);
  useExpiry(requests.nextChange(), expireGhosts);

  useEffect(() => setNodes((previous) => syncNodes(previous, graph, places)), [graph, places]);
  // Read once the new nodes are drawn, since placing them is what takes or drops the hint.
  useEffect(() => setHinted(places.held()), [nodes, places]);
  useEffect(() => () => clearTimeout(tidyTimer.current), []);

  const edges = useMemo<CanvasEdge[]>(() => {
    const routes = graph?.routes ?? [];
    const wires = routes.map((route): LevelFlowEdge => {
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
    });
    const ghosts = ghostWires(asked, routes).map(
      ({ id, ends, refused }): GhostFlowEdge => ({
        id,
        type: "ghost",
        source: ends.from.node,
        sourceHandle: handleId("out", ends.from.port),
        target: ends.to.node,
        targetHandle: handleId("in", ends.to.port),
        selectable: false,
        focusable: false,
        deletable: false,
        interactionWidth: 0,
        data: { refused },
      }),
    );
    return [...wires, ...ghosts];
  }, [graph, selectedEdges, asked]);

  const onNodesChange = useCallback(
    (changes: NodeChange<EarFlowNode>[]) => {
      setNodes((current) => applyNodeChanges(changes.filter((c) => c.type !== "remove" && c.type !== "add"), current));
      // A drag's end, or React Flow's keyboard move of a selected node: the place is kept.
      const moved = new Map<string, Position>();
      for (const change of changes) {
        if (change.type === "position" && change.dragging === false && change.position !== undefined)
          moved.set(change.id, change.position);
      }
      places.move(moved);
    },
    [places],
  );
  // A ghost cannot be selected, so no select change names one.
  const onEdgesChange = useCallback((changes: EdgeChange<CanvasEdge>[]) => {
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

  const hint = useCallback(
    (next: PlaceHint | null) => {
      places.hint(next);
      setHinted(next);
    },
    [places],
  );

  return useMemo(
    () => ({ nodes, edges, onNodesChange, onEdgesChange, selection, select, tidy, tidying, hint, hinted }),
    [nodes, edges, onNodesChange, onEdgesChange, selection, select, tidy, tidying, hint, hinted],
  );
}
