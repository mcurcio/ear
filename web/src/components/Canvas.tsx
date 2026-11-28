// The canvas (docs/editor.md#the-canvas): ear's own <ReactFlow> with ear's nodes and wires exactly as ear announced
// them (useCanvas), a drag from an output to an input as a route request (its handles' hints while drawn from
// patchbay's useConnectHints over connect.ts's rule, docs/editor.md#the-node-box), the delete key as removal requests
// that delete nothing themselves, the ghost wires of route requests on their way (docs/editor.md#requests-in-flight),
// a quiet dot grid, patchbay's CanvasTools with the minimap when shown (its boxes in their state's token, editor.css;
// a zoom tool or a move on the minimap stops the view following), patchbay's ConnectStatus at its bottom centre
// (why a port refuses a wire, from the same rule), patchbay's EmptyCanvasHint while ear's graph has no node, and the
// toasts at its bottom right (docs/editor.md#toasts). Quick add (docs/editor.md#adding-on-the-canvas) is
// patchbay's useAddAt: a double-click on empty canvas (in place of React Flow's zoom) or an output's wire dropped there
// opens the Add list at the pointer, with where the node should land; no route is asked for. A right-click, or a
// long-press on a touch screen, on a node or empty canvas is patchbay's useContextAt: the palette at that point, with
// the pressed node selected so its commands come first (docs/editor.md#adding-on-the-canvas). The view follows the
// graph through App's useFollowFit, whose handlers are merged here with the others.
import {
  CanvasTools,
  ConnectHints,
  ConnectStatus,
  EmptyCanvasHint,
  GhostNode,
  mergeFlowProps,
  useAddAt,
  useConnectHints,
  useContextAt,
  type FollowFitFlowProps,
  type GhostNodeNode,
  type OnAddAt,
  type OnContextAt,
} from "@mcurcio/patchbay/graph";
import {
  Background,
  BackgroundVariant,
  ReactFlow,
  useReactFlow,
  type Connection,
  type OnBeforeDelete,
  type OnNodesChange,
} from "@xyflow/react";
import { memo, useCallback, useMemo } from "react";
import { canConnect, quickAddKind } from "../connect";
import { describeHealth } from "../health";
import { shortcutKeys } from "../keys";
import { quickPlace, type Position } from "../layout";
import { ghostNode } from "../requests";
import type { RouteEnds } from "../protocol";
import type { Selection } from "../selection";
import { EarNode, type EarFlowNode } from "./EarNode";
import { GhostEdge } from "./GhostEdge";
import { LevelEdge, type LevelFlowEdge } from "./LevelEdge";
import { useRequests } from "./hooks";
import { Toasts } from "./Toasts";
import type { CanvasEdge, CanvasModel } from "./useCanvas";

const nodeTypes = { ear: EarNode, ghost: GhostNode };

/** A box on the canvas: one of ear's nodes, or the ghost of an add on its way. */
type CanvasNode = EarFlowNode | GhostNodeNode;
const edgeTypes = { level: LevelEdge, ghost: GhostEdge };

/** A minimap box takes its node's state class, so editor.css colours it from --state. */
const minimapClass = (node: CanvasNode) => (node.type === "ear" ? describeHealth(node.data.node).styleClass : "");

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
  onContext,
  follow,
  onFit,
  onHoldView,
  empty,
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
  /**
   * A right-click or a long-press: the screen point to open the palette at, the node pressed (none on empty canvas),
   * and where an Add from there should land.
   */
  onContext: (at: { x: number; y: number }, node: string | undefined, place: Position) => void;
  /** useFollowFit's handlers for <ReactFlow>: the person's own move, when it changes the view, stops following. */
  follow: FollowFitFlowProps;
  /** Fit, asked for by the person: fits and follows again. */
  onFit: () => void;
  /** The person moved the view by other means (a toast's Show, a zoom tool, the minimap): stop following. */
  onHoldView: () => void;
  /** ear's graph is here and holds no node (and no add is on its way): the empty canvas's hint shows. */
  empty: boolean;
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
  const onContextAt = useCallback<OnContextAt>(
    ({ point, at, node }) => onContext(at, node?.type === "ear" ? node.id : undefined, quickPlace(point, "pane")),
    [onContext],
  );
  const contextAt = useContextAt(onContextAt);

  // The handles' hints while a wire is drawn, and the drop's own check, by one rule; quick add and following beside it.
  const hints = useConnectHints(canConnect);
  const flowProps = useMemo(
    () => mergeFlowProps(hints.flowProps, addAt.flowProps, contextAt.flowProps, follow),
    [hints.flowProps, addAt.flowProps, contextAt.flowProps, follow],
  );

  // An add on its way, where quick add holds its place (docs/editor.md#requests-in-flight): a request, never a node, so
  // it cannot be selected, focused, deleted, dragged or connected. useCanvas's state does not hold it, so React Flow's
  // changes for it (its measured size) find no node there and change nothing.
  const ghost = ghostNode(useRequests(), canvas.hinted);
  const nodes = useMemo<CanvasNode[]>(() => {
    if (ghost === null) return canvas.nodes;
    const box: GhostNodeNode = {
      id: "ghost:add",
      type: "ghost",
      position: ghost.position,
      data: { title: ghost.title, line: "Adding…" },
      selectable: false,
      focusable: false,
      deletable: false,
      draggable: false,
      connectable: false,
    };
    return [...canvas.nodes, box];
  }, [canvas.nodes, ghost?.title, ghost?.position.x, ghost?.position.y]);

  // The delete key asks ear to remove the selection and deletes nothing itself; a ghost is a request, never a route.
  const onBeforeDelete = useCallback<OnBeforeDelete<CanvasNode, CanvasEdge>>(
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
      <ReactFlow<CanvasNode, CanvasEdge>
        {...flowProps}
        className={canvas.tidying ? "tidying" : undefined}
        nodes={nodes}
        edges={canvas.edges}
        nodeTypes={nodeTypes}
        edgeTypes={edgeTypes}
        onNodesChange={canvas.onNodesChange as OnNodesChange<CanvasNode>}
        onEdgesChange={canvas.onEdgesChange}
        onConnect={onConnect}
        onBeforeDelete={onBeforeDelete}
        onPaneClick={onPaneClick}
        deleteKeyCode={["Delete", "Backspace"]}
      >
        <Background variant={BackgroundVariant.Dots} gap={20} size={1} />
        <CanvasTools<CanvasNode>
          onFit={onFit}
          onViewMove={onHoldView}
          onTidy={canvas.tidy}
          minimap={minimap}
          onMinimap={onToggleMinimap}
          minimapNodeClassName={minimapClass}
          keyShortcuts={toolKeys}
        />
        <ConnectStatus value={hints.value} refused={hints.refused} />
        <EmptyCanvasHint empty={empty && ghost === null} keys={shortcutKeys.add}>
          <p>This graph has no nodes yet.</p>
        </EmptyCanvasHint>
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
