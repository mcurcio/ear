// The canvas (docs/editor.md#the-canvas): ear's nodes and wires exactly as ear announced them (useCanvas), a drag
// from an output to an input as a route request, the delete key as removal requests that delete nothing themselves,
// the ghost wires of route requests on their way (docs/editor.md#requests-in-flight), a quiet dot grid, the floating
// tools, the minimap when shown (its boxes in their state's token, editor.css), and the toasts at its bottom right
// (docs/editor.md#toasts). Quick add (docs/editor.md#adding-on-the-canvas): a double-click on empty canvas (in place of
// React Flow's zoom) or an output's wire dropped there opens the Add list at the pointer, with where the node should
// land; no route is asked for.
import {
  Background,
  BackgroundVariant,
  MiniMap,
  ReactFlow,
  useReactFlow,
  type Connection,
  type OnBeforeDelete,
  type OnConnectEnd,
} from "@xyflow/react";
import { memo, useCallback, type MouseEvent } from "react";
import { droppedOnPane, isValidConnection } from "../connect";
import { describeHealth } from "../health";
import { quickPlace, type Position } from "../layout";
import type { RouteEnds } from "../protocol";
import type { Selection } from "../selection";
import { CanvasTools } from "./CanvasTools";
import { EarNode, type EarFlowNode } from "./EarNode";
import { GhostEdge } from "./GhostEdge";
import { LevelEdge, type LevelFlowEdge } from "./LevelEdge";
import { Toasts } from "./Toasts";
import type { CanvasEdge, CanvasModel } from "./useCanvas";

const nodeTypes = { ear: EarNode };
const edgeTypes = { level: LevelEdge, ghost: GhostEdge };

/** A minimap box takes its node's state class, so editor.css colours it from --state. */
const minimapClass = (node: EarFlowNode) => describeHealth(node.data.node).styleClass;

// Memoised: App draws again on every keystroke in the drawer, and the boxes need not.
export const Canvas = memo(function Canvas({
  canvas,
  onConnect,
  onRemove,
  onPaneClick,
  minimap,
  onToggleMinimap,
  onShow,
  onQuickAdd,
}: {
  canvas: CanvasModel;
  onConnect: (connection: Connection) => void;
  onRemove: (nodes: readonly string[], routes: readonly RouteEnds[]) => void;
  onPaneClick: () => void;
  minimap: boolean;
  onToggleMinimap: () => void;
  /** A toast's Show: select its node or route. */
  onShow: (selection: Selection) => void;
  /** Quick add: the screen point to open the Add list at, and where on the flow the node should land. */
  onQuickAdd: (at: { x: number; y: number }, place: Position) => void;
}) {
  const { screenToFlowPosition } = useReactFlow();
  const onDoubleClick = useCallback(
    (event: MouseEvent) => {
      // Only the empty pane: a double-click on a box or a wire is theirs.
      if (!(event.target instanceof Element) || !event.target.classList.contains("react-flow__pane")) return;
      const at = { x: event.clientX, y: event.clientY };
      onQuickAdd(at, quickPlace(screenToFlowPosition(at), "pane"));
    },
    [onQuickAdd, screenToFlowPosition],
  );
  const onConnectEnd = useCallback<OnConnectEnd>(
    (event, state) => {
      if (!droppedOnPane(state)) return;
      const point = "changedTouches" in event ? event.changedTouches[0] : event;
      if (point === undefined) return;
      const at = { x: point.clientX, y: point.clientY };
      onQuickAdd(at, quickPlace(screenToFlowPosition(at), "wire"));
    },
    [onQuickAdd, screenToFlowPosition],
  );

  // The delete key asks ear to remove the selection and deletes nothing itself; a ghost is a request, never a route.
  const onBeforeDelete = useCallback<OnBeforeDelete<EarFlowNode, CanvasEdge>>(
    async ({ nodes, edges }) => {
      onRemove(
        nodes.map((node) => node.id),
        edges.filter((edge): edge is LevelFlowEdge => edge.type === "level").map((edge) => edge.data!.route),
      );
      return false;
    },
    [onRemove],
  );

  return (
    <ReactFlow<EarFlowNode, CanvasEdge>
      className={canvas.tidying ? "tidying" : undefined}
      nodes={canvas.nodes}
      edges={canvas.edges}
      nodeTypes={nodeTypes}
      edgeTypes={edgeTypes}
      onNodesChange={canvas.onNodesChange}
      onEdgesChange={canvas.onEdgesChange}
      onConnect={onConnect}
      onConnectEnd={onConnectEnd}
      isValidConnection={isValidConnection}
      onBeforeDelete={onBeforeDelete}
      onPaneClick={onPaneClick}
      onDoubleClick={onDoubleClick}
      zoomOnDoubleClick={false}
      deleteKeyCode={["Delete", "Backspace"]}
      fitView
    >
      <Background variant={BackgroundVariant.Dots} gap={20} size={1} />
      <CanvasTools minimap={minimap} onToggleMinimap={onToggleMinimap} onTidy={canvas.tidy} />
      {minimap && (
        <MiniMap<EarFlowNode> position="top-right" pannable zoomable ariaLabel="Minimap" nodeClassName={minimapClass} />
      )}
      <Toasts onShow={onShow} />
    </ReactFlow>
  );
});
