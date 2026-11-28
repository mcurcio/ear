// The Node subject's ports, each with a "Connect to" choice (docs/editor.md#the-inspector): the keyboard's way to draw
// a route, listing the ports on the other side that the canvas's own rule accepts (connect.ts's portTargets over
// patchbay's connectTargets and connectRule), so the choice never offers what a drag would refuse, a route already held
// among it. Choosing one sends route.add, as a drop does; the route appears when ear announces it, and its ghost wire
// shows while it is on its way.
import { Field, Select } from "@mcurcio/patchbay/primitives";
import { portTargets, type Direction } from "../connect";
import type { Graph, GraphNode } from "../protocol";
import { useEditor } from "./hooks";

function PortChoice({ graph, node, direction, port }: { graph: Graph; node: string; direction: Direction; port: string }) {
  const editor = useEditor();
  const targets = portTargets(graph, node, direction, port);
  const side = direction === "in" ? "input" : "output";
  return (
    <Field label={`Connect ${side} ${port} to`}>
      <Select
        font="code"
        value=""
        disabled={targets.length === 0}
        onChange={(event) => {
          const target = targets.find((t) => t.key === event.target.value);
          if (target !== undefined) editor.send("route.add", target.ends);
        }}
      >
        <option value="">{targets.length === 0 ? "Nothing to connect to" : "Connect to…"}</option>
        {targets.map((target) => (
          <option key={target.key} value={target.key}>
            {target.label}
          </option>
        ))}
      </Select>
    </Field>
  );
}

export function PortConnect({ graph, node }: { graph: Graph; node: GraphNode }) {
  if (node.inputs.length === 0 && node.outputs.length === 0) return null;
  return (
    <div className="port-connect">
      <h3>Ports</h3>
      {node.inputs.map((port) => (
        <PortChoice key={`in ${port}`} graph={graph} node={node.name} direction="in" port={port} />
      ))}
      {node.outputs.map((port) => (
        <PortChoice key={`out ${port}`} graph={graph} node={node.name} direction="out" port={port} />
      ))}
    </div>
  );
}
