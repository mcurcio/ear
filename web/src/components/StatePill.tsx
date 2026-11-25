// A node's state pill (docs/editor.md#the-node-box): a dot and the word, with a failure's code in monospace. The node
// box and the inspector's Node subject both draw it, so both say the state the same way. Its colour is --state, set by
// an ancestor carrying the health's style class.
import type { HealthView } from "../health";
import type { GraphNode } from "../protocol";

export function StatePill({ node, health }: { node: GraphNode; health: HealthView }) {
  return (
    <span className="ear-node-pill" title={node.reason?.detail}>
      <span className="ear-node-pill-dot" aria-hidden="true" />
      {health.word}
      {node.reason !== undefined && <code className="ear-node-reason">{node.reason.code}</code>}
    </span>
  );
}
