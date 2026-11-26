// A route's wire (docs/editor.md#wires): the stroke in its status's class, sized and brightened by its from port's
// level; a flow over it while a linked wire carries signal; a hit path whose tooltip names the two ends; and the
// status in words at the midpoint where colour is not enough. Its rules are wires.ts's; its colours are editor.css's
// wire classes over patchbay's tokens (docs/editor.md#tokens-and-themes). The visible path keeps React Flow's
// react-flow__edge-path class and the hit path its react-flow__edge-interaction, so selection and Delete work as
// before.
import { EdgeLabelRenderer, getBezierPath, type Edge, type EdgeProps } from "@xyflow/react";
import type { CSSProperties } from "react";
import { wirePort } from "../levels";
import type { GraphRoute } from "../protocol";
import { wireView } from "../wires";
import { useLevel } from "./hooks";

export type LevelEdgeData = { route: GraphRoute };
export type LevelFlowEdge = Edge<LevelEdgeData, "level">;

export function LevelEdge(props: EdgeProps<LevelFlowEdge>) {
  const { sourceX, sourceY, sourcePosition, targetX, targetY, targetPosition, data, selected } = props;
  const route = data!.route;
  const port = wirePort(route);
  const [, rms] = useLevel(port.node, port.port);
  const view = wireView(route, rms, selected ?? false);
  const [path, labelX, labelY] = getBezierPath({ sourceX, sourceY, sourcePosition, targetX, targetY, targetPosition });
  const width = { "--wire-width": `${view.width}px` } as CSSProperties;
  return (
    <>
      <path
        id={props.id}
        d={path}
        fill="none"
        className={`react-flow__edge-path ear-wire ${view.statusClass}${view.dashed ? " ear-wire-dashed" : ""}`}
        style={{ ...width, strokeOpacity: view.opacity }}
      />
      {view.flowing && <path d={path} fill="none" className={`ear-wire-flow ${view.statusClass}`} style={width} />}
      <path d={path} fill="none" className="react-flow__edge-interaction" strokeOpacity={0} strokeWidth={16}>
        <title>{view.title}</title>
      </path>
      {view.label !== undefined && (
        <EdgeLabelRenderer>
          <div
            className={`wire-label ${view.statusClass}`}
            style={{ transform: `translate(-50%, -50%) translate(${labelX}px, ${labelY}px)` }}
          >
            {view.label}
          </div>
        </EdgeLabelRenderer>
      )}
    </>
  );
}
