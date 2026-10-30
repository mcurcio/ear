// A node box: its name, type and state (with the reason when it failed), then one row per port,
// inputs on the left and outputs on the right, each with its handle and its level meter.
import { Handle, Position, type Node, type NodeProps } from "@xyflow/react";
import { handleId } from "../connect";
import type { GraphNode } from "../protocol";
import { Meter } from "./Meter";

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
  return (
    <div className={`ear-node state-${node.state}${selected ? " selected" : ""}`}>
      <div className="ear-node-header">
        <strong>{node.name}</strong> <span className="ear-node-type">{node.type}</span>
        <div className="ear-node-state" title={node.reason?.detail}>
          {node.state}
          {node.reason !== undefined && `: ${node.reason.code}`}
        </div>
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
    </div>
  );
}
