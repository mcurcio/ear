// A node box (docs/editor.md#the-node-box): patchbay's NodeBox, its stripe and its state pill in the state's tone
// (health.ts's stateTone), with the counters' mark after the pill while a loss counter of its own rose in the last
// minute (counters.ts, docs/editor.md#counters); its name with its type under it, as sent; the line that says why it is
// silent or what it waits for (health.ts, docs/editor.md#a-nodes-health); then its ports stacked (layout.ts's
// nodePorts), the inputs' rows with their handles on the left and then the outputs' on the right, each a PortRow with its handle (connect.ts's handle id) and its level meter in the
// trailing slot, in every phase; and in the footer the strip of its companions' badges (companions.ts,
// docs/editor.md#companions), outside every handle. While a wire is drawn each PortRow takes its hint from the canvas's
// ConnectHints (patchbay's useConnectHints over connect.ts's connectRule, the drop rule itself).
import { NodeBox, type PortSpec } from "@mcurcio/patchbay/graph";
import type { Node, NodeProps } from "@xyflow/react";
import { useEffect, useState } from "react";
import { handleId, type Direction } from "../connect";
import { describeHealth, startingLineDelayMs, stateTone } from "../health";
import { nodePorts } from "../layout";
import type { GraphNode } from "../protocol";
import { CompanionStrip } from "./CompanionStrip";
import { NodeCounterMark } from "./Counters";
import { useCompanionBadges, useNodeCounters } from "./hooks";
import { PortMeter } from "./Meter";
import { StatePill } from "./StatePill";

/**
 * How long this box has shown the node in its phase, on the browser's clock from when the phase (or a new since,
 * a phase entered again) arrived; the box is drawn again once a starting phase's hidden line is due.
 */
function usePhaseSeen(phase: string, since: string | undefined): number {
  const [seen, setSeen] = useState(() => ({ phase, since, at: Date.now() }));
  const [, redraw] = useState(0);
  const current = seen.phase === phase && seen.since === since ? seen : { phase, since, at: Date.now() };
  useEffect(() => {
    if (current !== seen) setSeen(current);
    const due = current.at + startingLineDelayMs - Date.now();
    if (!(due > 0)) return undefined;
    const timer = setTimeout(() => redraw((n) => n + 1), due);
    return () => clearTimeout(timer);
  }, [current, seen]);
  return Date.now() - current.at;
}

export type EarNodeData = { node: GraphNode };
export type EarFlowNode = Node<EarNodeData, "ear">;

function ports(node: GraphNode, direction: Direction): PortSpec[] {
  return (direction === "in" ? node.inputs : node.outputs).map((port) => {
    const id = handleId(direction, port);
    return {
      id,
      label: port,
      trailing: <PortMeter node={node.name} port={port} label={`${port} level`} />,
    };
  });
}

export function EarNode({ data: { node }, selected }: NodeProps<EarFlowNode>) {
  const health = describeHealth(node, usePhaseSeen(node.phase, node.since));
  const companions = useCompanionBadges(node);
  const { mark } = useNodeCounters(node);
  // A change of ports redraws the same box (docs/editor.md#the-node-box): NodeBox has React Flow measure its handles
  // again, so a surviving port's wires stay on it and a new port's handle takes wires.
  return (
    <NodeBox
      title={node.name}
      subtitle={node.type}
      tone={stateTone(node)}
      disabled={node.state === "disabled"}
      failed={node.state === "failed"}
      status={
        <>
          <StatePill node={node} health={health} />
          <NodeCounterMark mark={mark} />
        </>
      }
      line={health.line === "" ? undefined : health.line}
      inputs={ports(node, "in")}
      outputs={ports(node, "out")}
      ports={nodePorts}
      footer={companions.length === 0 ? undefined : <CompanionStrip badges={companions} />}
      selected={selected}
    />
  );
}
