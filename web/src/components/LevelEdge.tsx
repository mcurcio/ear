// A route's wire: coloured by its status (linked, pending, refused) and drawn by its level, which
// is its from port's (docs/design/batch2.md section 6): louder is thicker and brighter.
import { BaseEdge, getBezierPath, type Edge, type EdgeProps } from "@xyflow/react";
import { meterFraction, wirePort } from "../levels";
import type { GraphRoute } from "../protocol";
import { useLevel } from "./hooks";

export type LevelEdgeData = { route: GraphRoute };
export type LevelFlowEdge = Edge<LevelEdgeData, "level">;

const statusColour = { linked: "#2f9e44", pending: "#e8a317", refused: "#e03131" } as const;

export function LevelEdge(props: EdgeProps<LevelFlowEdge>) {
  const { sourceX, sourceY, sourcePosition, targetX, targetY, targetPosition, data, selected } = props;
  const route = data!.route;
  const port = wirePort(route);
  const [, rms] = useLevel(port.node, port.port);
  const level = meterFraction(rms);
  const [path] = getBezierPath({ sourceX, sourceY, sourcePosition, targetX, targetY, targetPosition });
  return (
    <BaseEdge
      id={props.id}
      path={path}
      interactionWidth={16}
      style={{
        stroke: statusColour[route.status],
        strokeWidth: 1.5 + 5 * level + (selected ? 1.5 : 0),
        strokeOpacity: 0.45 + 0.55 * level,
        strokeDasharray: route.status === "linked" ? undefined : "6 4",
      }}
    />
  );
}
