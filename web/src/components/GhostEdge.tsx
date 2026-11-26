// A route request's ghost wire while it is on its way (docs/editor.md#requests-in-flight): plainly a request, never a
// route. One thin dashed --pb-muted path between the two handles, turning --pb-err for a second when ear refuses it; no
// status, no meter, no tooltip and no hit path. It has no react-flow__edge-path class, so React Flow's stroke and
// selection styles never reach it, and useCanvas makes its edge unselectable, unfocusable and undeletable.
import { getBezierPath, type Edge, type EdgeProps } from "@xyflow/react";

export type GhostEdgeData = { refused: boolean };
export type GhostFlowEdge = Edge<GhostEdgeData, "ghost">;

export function GhostEdge(props: EdgeProps<GhostFlowEdge>) {
  const { sourceX, sourceY, sourcePosition, targetX, targetY, targetPosition, data } = props;
  const [path] = getBezierPath({ sourceX, sourceY, sourcePosition, targetX, targetY, targetPosition });
  return (
    <path
      d={path}
      fill="none"
      className={data?.refused === true ? "ear-ghost ear-ghost-refused" : "ear-ghost"}
      aria-hidden="true"
    />
  );
}
