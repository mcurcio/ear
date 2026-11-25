// The inspector's Route subject (docs/editor.md#the-inspector): its two ends as node:port buttons that select their
// node, its status in words (wires.ts's wireStatusWords, its colour editor.css's over the state tokens, the word
// always shown), its from port's level meter, and Remove, which is a request to ear.
import { wirePort } from "../levels";
import type { GraphRoute, PortRef, RouteEnds } from "../protocol";
import type { Selection } from "../selection";
import { wireStatusWords, wireView } from "../wires";
import { Meter } from "./Meter";

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
      <dl className="facts">
        <div>
          <dt>From</dt>
          <dd>{end(route.from)}</dd>
        </div>
        <div>
          <dt>To</dt>
          <dd>{end(route.to)}</dd>
        </div>
        <div>
          <dt>Status</dt>
          <dd className={`wire-status ${wireView(route, 0, false).statusClass}`}>{wireStatusWords(route)}</dd>
        </div>
        <div>
          <dt>Level</dt>
          <dd className="route-level">
            <Meter node={level.node} port={level.port} />
          </dd>
        </div>
      </dl>
      <button type="button" className="inspector-delete" onClick={() => onRemove(route)}>
        Remove route
      </button>
    </section>
  );
}
