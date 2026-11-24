// The canvas (docs/editor.md#the-canvas): ear's nodes and wires exactly as ear announced them (useCanvas), a drag
// from an output to an input as a route request, the delete key as removal requests that delete nothing themselves,
// the ghost wires of route requests on their way (docs/editor.md#requests-in-flight), a quiet dot grid, the floating
// tools, the minimap when shown (its boxes in their state's token, editor.css), and the toasts at its bottom right
// (docs/editor.md#toasts).
import {
  Background,
  BackgroundVariant,
  MiniMap,
  ReactFlow,
  type Connection,
  type OnBeforeDelete,
} from "@xyflow/react";
import { memo, useCallback } from "react";
import { isValidConnection } from "../connect";
import { describeHealth } from "../health";
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
}: {
  canvas: CanvasModel;
  onConnect: (connection: Connection) => void;
  onRemove: (nodes: readonly string[], routes: readonly RouteEnds[]) => void;
  onPaneClick: () => void;
  minimap: boolean;
  onToggleMinimap: () => void;
  /** A toast's Show: select its node or route. */
  onShow: (selection: Selection) => void;
}) {
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
      isValidConnection={isValidConnection}
      onBeforeDelete={onBeforeDelete}
      onPaneClick={onPaneClick}
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
