// The canvas (docs/editor.md#the-canvas): ear's nodes and wires exactly as ear announced them (useCanvas), a drag
// from an output to an input as a route request (its handles' hints while drawn from patchbay's useConnectHints over
// connect.ts's rule, docs/editor.md#the-node-box), the delete key as removal requests that delete nothing themselves,
// the ghost wires of route requests on their way (docs/editor.md#requests-in-flight), a quiet dot grid, the floating
// tools, the minimap when shown (its boxes in their state's token, editor.css), and the toasts at its bottom right
// (docs/editor.md#toasts). Quick add (docs/editor.md#adding-on-the-canvas): a double-click on empty canvas (in place of
// React Flow's zoom) or an output's wire dropped there opens the Add list at the pointer, with where the node should
// land; no route is asked for.
import { ConnectHints, mergeFlowProps, useConnectHints } from "@mcurcio/patchbay/graph";
import {
  Background,
  BackgroundVariant,
  MiniMap,
  ReactFlow,
  useNodesInitialized,
  useReactFlow,
  useStore,
  type Connection,
  type OnBeforeDelete,
  type OnConnectEnd,
} from "@xyflow/react";
import { memo, useCallback, useEffect, useMemo, type MouseEvent } from "react";
import { canConnect, droppedOnPane } from "../connect";
import { fitKey, type AutoFit } from "../fit";
import { describeHealth } from "../health";
import { quickPlace, type Position } from "../layout";
import type { RouteEnds } from "../protocol";
import type { Selection } from "../selection";
import { CanvasTools, fitPadding } from "./CanvasTools";
import { EarNode, type EarFlowNode } from "./EarNode";
import { GhostEdge } from "./GhostEdge";
import { LevelEdge, type LevelFlowEdge } from "./LevelEdge";
import { Toasts } from "./Toasts";
import type { CanvasEdge, CanvasModel } from "./useCanvas";

const nodeTypes = { ear: EarNode };
const edgeTypes = { level: LevelEdge, ghost: GhostEdge };

/** A minimap box takes its node's state class, so editor.css colours it from --state. */
const minimapClass = (node: EarFlowNode) => describeHealth(node.data.node).styleClass;

/**
 * Fits the view by itself while the person has not moved it (fit.ts, docs/editor.md#the-canvas): once the boxes are
 * measured, and whenever the nodes on the canvas or its size change.
 */
function AutoFitter({ autoFit }: { autoFit: AutoFit }) {
  const { fitView } = useReactFlow();
  const measured = useNodesInitialized();
  const key = useStore((state) => fitKey(state.width, state.height, state.nodes.map((node) => node.id)));
  useEffect(() => {
    if (measured && autoFit.due(key)) void fitView({ padding: fitPadding });
  }, [measured, key, autoFit, fitView]);
  return null;
}

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
  autoFit,
  onFit,
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
  /** When the view fits by itself (fit.ts); a move by the person holds it. */
  autoFit: AutoFit;
  /** Fit, asked for by the person. */
  onFit: () => void;
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

  // The handles' hints while a wire is drawn, and the drop's own check, by one rule; quick add's handler beside it.
  const hints = useConnectHints(canConnect);
  const flowProps = useMemo(() => mergeFlowProps(hints.flowProps, { onConnectEnd }), [hints.flowProps, onConnectEnd]);

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
    <ConnectHints value={hints.value}>
      <ReactFlow<EarFlowNode, CanvasEdge>
        {...flowProps}
        className={canvas.tidying ? "tidying" : undefined}
        nodes={canvas.nodes}
        edges={canvas.edges}
        nodeTypes={nodeTypes}
        edgeTypes={edgeTypes}
        onNodesChange={canvas.onNodesChange}
        onEdgesChange={canvas.onEdgesChange}
        onConnect={onConnect}
        onBeforeDelete={onBeforeDelete}
        onPaneClick={onPaneClick}
        onDoubleClick={onDoubleClick}
        zoomOnDoubleClick={false}
        deleteKeyCode={["Delete", "Backspace"]}
        onMoveStart={(event) => {
          // A move with an event is the person's (a pan, a zoom); a fit's own move has none.
          if (event !== null) autoFit.hold();
        }}
      >
        <AutoFitter autoFit={autoFit} />
        <Background variant={BackgroundVariant.Dots} gap={20} size={1} />
        <CanvasTools minimap={minimap} onToggleMinimap={onToggleMinimap} onTidy={canvas.tidy} onFit={onFit} />
        {minimap && (
          <MiniMap<EarFlowNode> position="top-right" pannable zoomable ariaLabel="Minimap" nodeClassName={minimapClass} />
        )}
        <Toasts
          onShow={(selection) => {
            // Show moves the view to the toast's subject: the person's own move.
            autoFit.hold();
            onShow(selection);
          }}
        />
      </ReactFlow>
    </ConnectHints>
  );
});
