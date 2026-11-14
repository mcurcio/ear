// A node box: its name, type and state word with the line that says why it is silent or what it
// waits for (health.ts, docs/editor.md#a-nodes-health), then one row per port, inputs on the left and outputs
// on the right, each with its handle and its level meter, in every phase; and along its bottom edge the strip of its
// companions' badges (companions.ts, docs/editor.md#companions), outside every handle.
import { Handle, Position, type Node, type NodeProps } from "@xyflow/react";
import { useEffect, useState } from "react";
import { handleId } from "../connect";
import { describeHealth, startingLineDelayMs } from "../health";
import type { GraphNode } from "../protocol";
import { CompanionStrip } from "./CompanionStrip";
import { useCompanionBadges } from "./hooks";
import { Meter } from "./Meter";

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
  return (
    <div className={`port port-${direction}`}>
      <Handle
        type={isInput ? "target" : "source"}
        position={isInput ? Position.Left : Position.Right}
        id={handleId(direction, port)}
      />
      <span className="port-name">{port}</span>
      <Meter node={node} port={port} />
    </div>
  );
}

export function EarNode({ data: { node }, selected }: NodeProps<EarFlowNode>) {
  const health = describeHealth(node, usePhaseSeen(node.phase, node.since));
  const companions = useCompanionBadges(node);
  return (
    <div className={`ear-node ${health.styleClass} phase-${node.phase}${selected ? " selected" : ""}`}>
      <div className="ear-node-header">
        <strong>{node.name}</strong> <span className="ear-node-type">{node.type}</span>
        <div className="ear-node-state" title={node.reason?.detail}>
          {health.word}
          {node.reason !== undefined && `: ${node.reason.code}`}
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
