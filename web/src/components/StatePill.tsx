// A node's state pill (docs/editor.md#the-node-box): patchbay's Pill, a dot and the word in the state's tone
// (health.ts's stateTone), with a failure's code after the word and the dot pulsing while the node configures. The
// node box and the inspector's Node subject both draw it, so both say the state the same way.
import { Pill } from "@mcurcio/patchbay/primitives";
import { stateTone, statePulses, type HealthView } from "../health";
import type { GraphNode } from "../protocol";

export function StatePill({ node, health }: { node: GraphNode; health: HealthView }) {
  return (
    <Pill
      className="ear-node-pill"
      tone={stateTone(node)}
      label={health.word}
      pulse={statePulses(node)}
      title={node.reason?.detail}
      detail={node.reason?.code}
    />
  );
}
