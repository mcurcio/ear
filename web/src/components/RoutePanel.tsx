// The inspector's Route subject (docs/editor.md#the-inspector): its two ends as node:port buttons that select their
// node, its status in words (wires.ts's wireStatusWords, its colour editor.css's over the state tokens, the word
// always shown), its from port's level meter, and Remove, which is a request to ear.
import { Button, KeyValue } from "@mcurcio/patchbay/primitives";
import { wirePort } from "../levels";
import type { GraphRoute, PortRef, RouteEnds } from "../protocol";
import type { Selection } from "../selection";
import { wireStatusClass, wireStatusWords } from "../wires";
import { PortMeter } from "./Meter";

export function RoutePanel({
  route,
  onSelect,
  onRemove,
}: {
  route: GraphRoute;
  onSelect: (selection: Selection) => void;
  onRemove: (route: RouteEnds) => void;
}) {
  const level = wirePort(route);
  const end = (port: PortRef) => (
    <button
      type="button"
      className="port-ref"
      title={`Select ${port.node}`}
      onClick={() => onSelect({ nodes: [port.node], routes: [] })}
    >
      {port.node}:{port.port}
    </button>
  );
  return (
    <section className="inspector-section">
      <KeyValue
        className="facts"
        rows={[
          { key: "From", value: end(route.from) },
          { key: "To", value: end(route.to) },
          {
            key: "Status",
            value: (
              <span className={`wire-status ${wireStatusClass(route)}`}>{wireStatusWords(route)}</span>
            ),
          },
          {
            key: "Level",
            value: (
              <span className="route-level">
                <PortMeter node={level.node} port={level.port} label={`${level.node}:${level.port} level`} />
              </span>
            ),
          },
        ]}
      />
      <Button tone="danger" className="inspector-delete" onClick={() => onRemove(route)}>
        Remove route
      </Button>
    </section>
  );
}
