// A route's wire (docs/editor.md#wires): patchbay's Wire, given what wires.ts makes of the route and its from port's
// level. Each level message redraws only this edge, and Wire keeps its path: the level reaches the stroke as a CSS
// variable, so it is restyled, never rebuilt. React Flow's edge classes stay Wire's, so selection and Delete work.
import { Wire, type WireEdge } from "@mcurcio/patchbay/graph";
import type { Edge, EdgeProps } from "@xyflow/react";
import { wirePort } from "../levels";
import type { GraphRoute } from "../protocol";
import { wireData } from "../wires";
import { useLevel } from "./hooks";

export type LevelEdgeData = { route: GraphRoute };
export type LevelFlowEdge = Edge<LevelEdgeData, "level">;

export function LevelEdge(props: EdgeProps<LevelFlowEdge>) {
  const route = props.data!.route;
  const port = wirePort(route);
  const [, rms] = useLevel(port.node, port.port);
  const wire: EdgeProps<WireEdge> = { ...props, type: "wire", data: wireData(route, rms) };
  return <Wire {...wire} />;
}
