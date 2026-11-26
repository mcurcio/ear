// The canvas (docs/editor.md#the-canvas): ear's own <ReactFlow> with ear's nodes and wires exactly as ear announced
// them (useCanvas), a drag from an output to an input as a route request (its handles' hints while drawn from
// patchbay's useConnectHints over connect.ts's rule, docs/editor.md#the-node-box), the delete key as removal requests
// that delete nothing themselves, the ghost wires of route requests on their way (docs/editor.md#requests-in-flight),
// a quiet dot grid, patchbay's CanvasTools with the minimap when shown (its boxes in their state's token, editor.css),
// and the toasts at its bottom right (docs/editor.md#toasts). Quick add (docs/editor.md#adding-on-the-canvas) is
// patchbay's useAddAt: a double-click on empty canvas (in place of React Flow's zoom) or an output's wire dropped there
// opens the Add list at the pointer, with where the node should land; no route is asked for. The view follows the
// graph through App's useFollowFit, whose handler is merged here with the others.
import {
  CanvasTools,
  ConnectHints,
  mergeFlowProps,
  useAddAt,
  useConnectHints,
  type FollowFitFlowProps,
  type OnAddAt,
} from "@mcurcio/patchbay/graph";
import {
  Background,
  BackgroundVariant,
  ReactFlow,
  useReactFlow,
  type Connection,
  type OnBeforeDelete,
} from "@xyflow/react";
import { memo, useCallback, useMemo } from "react";
import { canConnect, quickAddKind } from "../connect";
import { describeHealth } from "../health";
import { quickPlace, type Position } from "../layout";
import type { RouteEnds } from "../protocol";
import type { Selection } from "../selection";
import { EarNode, type EarFlowNode } from "./EarNode";
import { GhostEdge } from "./GhostEdge";
import { LevelEdge, type LevelFlowEdge } from "./LevelEdge";
import { Toasts } from "./Toasts";
import type { CanvasEdge, CanvasModel } from "./useCanvas";

const nodeTypes = { ear: EarNode };
const edgeTypes = { level: LevelEdge, ghost: GhostEdge };

/** A minimap box takes its node's state class, so editor.css colours it from --state. */
const minimapClass = (node: EarFlowNode) => describeHealth(node.data.node).styleClass;

/** The tools' keys (docs/editor.md#shortcuts), shown on them; keys.ts binds them. */
const toolKeys = { fit: "f", tidy: "l", minimap: "m" };

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
  follow,
  onFit,
  onHoldView,
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
  /** useFollowFit's handler for <ReactFlow>: the person's own move stops the view following the graph. */
  follow: FollowFitFlowProps;
  /** Fit, asked for by the person: fits and follows again. */
  onFit: () => void;
  /** The person moved the view by other means (a toast's Show): stop following. */
  onHoldView: () => void;
}) {
  const { flowToScreenPosition } = useReactFlow();
  const onAddAt = useCallback<OnAddAt>(
    (point, from) => {
      const kind = quickAddKind(from);
      if (kind === null) return;
      onQuickAdd(flowToScreenPosition(point), quickPlace(point, kind));
    },
    [onQuickAdd, flowToScreenPosition],
  );
  const addAt = useAddAt(onAddAt);

  // The handles' hints while a wire is drawn, and the drop's own check, by one rule; quick add and following beside it.
  const hints = useConnectHints(canConnect);
  const flowProps = useMemo(
    () => mergeFlowProps(hints.flowProps, addAt.flowProps, follow),
    [hints.flowProps, addAt.flowProps, follow],
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
        deleteKeyCode={["Delete", "Backspace"]}
      >
        <Background variant={BackgroundVariant.Dots} gap={20} size={1} />
        <CanvasTools<EarFlowNode>
          onFit={onFit}
          onTidy={canvas.tidy}
          minimap={minimap}
          onMinimap={onToggleMinimap}
          minimapNodeClassName={minimapClass}
          keyShortcuts={toolKeys}
        />
        <Toasts
          onShow={(selection) => {
            // Show moves the view to the toast's subject: the person's own move.
            onHoldView();
            onShow(selection);
          }}
        />
      </ReactFlow>
    </ConnectHints>
  );
});
