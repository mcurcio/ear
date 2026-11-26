// A route request's ghost wire while it is on its way (docs/editor.md#requests-in-flight): patchbay's GhostWire,
// plainly a request, never a route, in the error tone for the second requests.ts keeps a refused one. useCanvas makes
// its edge unselectable, unfocusable and undeletable.
import { GhostWire, type GhostWireEdge } from "@mcurcio/patchbay/graph";
import type { Edge, EdgeProps } from "@xyflow/react";

export type GhostEdgeData = { refused: boolean };
export type GhostFlowEdge = Edge<GhostEdgeData, "ghost">;

export function GhostEdge(props: EdgeProps<GhostFlowEdge>) {
  const tone = props.data?.refused === true ? "refused" : "pending";
  const ghost: EdgeProps<GhostWireEdge> = { ...props, data: { tone } };
  return <GhostWire {...ghost} />;
}
