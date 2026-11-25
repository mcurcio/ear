// A node box (docs/editor.md#the-node-box): a stripe down its left edge and a pill (a dot and the word) in its state's
// colour, with the counters' mark after the pill while a loss counter of its own rose in the last minute (counters.ts,
// docs/editor.md#counters); its name with its type as a small label, as sent; the line that says why it is silent or
// what it waits for (health.ts, docs/editor.md#a-nodes-health); then one row per port, inputs on the left and outputs
// on the right, each with its handle and its level meter, in every phase; and along its bottom edge the strip of its
// companions' badges (companions.ts, docs/editor.md#companions), outside every handle. While a wire is drawn each
// handle takes its cue from connect.ts's handleCue, the drop rule itself.
// React Flow's useConnection is a drag in progress; hooks.ts's is the editor's socket.
import { Handle, Position, useConnection as useFlowConnection, type Node, type NodeProps } from "@xyflow/react";
import { useEffect, useState } from "react";
import { handleCue, handleId } from "../connect";
import { describeHealth, startingLineDelayMs } from "../health";
import type { GraphNode } from "../protocol";
import { CompanionStrip } from "./CompanionStrip";
import { NodeCounterMark } from "./Counters";
import { useCompanionBadges, useNodeCounters } from "./hooks";
import { Meter } from "./Meter";
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

function Port({ node, port, direction }: { node: string; port: string; direction: "in" | "out" }) {
  const isInput = direction === "in";
  const id = handleId(direction, port);
  // A string selector, so a pointer move during a drag redraws only the handles whose cue changed.
  const cue = useFlowConnection((connection) => {
    const from = connection.inProgress ? { nodeId: connection.fromHandle.nodeId, id: connection.fromHandle.id } : null;
    return handleCue(from, node, id);
  });
  return (
    <div className={`port port-${direction}`}>
      <Handle
        type={isInput ? "target" : "source"}
        position={isInput ? Position.Left : Position.Right}
        id={id}
        className={`handle-${cue}`}
      />
      <span className="port-name">{port}</span>
      <Meter node={node} port={port} />
    </div>
  );
}

export function EarNode({ data: { node }, selected }: NodeProps<EarFlowNode>) {
  const health = describeHealth(node, usePhaseSeen(node.phase, node.since));
  const companions = useCompanionBadges(node);
  const { mark } = useNodeCounters(node);
  return (
    <div className={`ear-node ${health.styleClass} phase-${node.phase}${selected ? " selected" : ""}`}>
      <div className="ear-node-header">
        <div className="ear-node-title">
          <strong className="ear-node-name" title={node.name}>
            {node.name}
          </strong>
          <span className="ear-node-type">{node.type}</span>
        </div>
        <div className="ear-node-status">
          <StatePill node={node} health={health} />
          <NodeCounterMark mark={mark} />
        </div>
        {health.line !== "" && (
          <div className="ear-node-line" title={health.line}>
            {health.line}
          </div>
        )}
      </div>
      <div className="ear-node-ports">
        <div className="ports-in">
          {node.inputs.map((port) => (
            <Port key={port} node={node.name} port={port} direction="in" />
          ))}
        </div>
        <div className="ports-out">
          {node.outputs.map((port) => (
            <Port key={port} node={node.name} port={port} direction="out" />
          ))}
        </div>
      </div>
      <CompanionStrip badges={companions} />
    </div>
  );
}
