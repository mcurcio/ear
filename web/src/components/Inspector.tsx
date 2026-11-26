// The inspector (docs/editor.md#the-inspector): one subject, chosen by the selection (selection.ts): the Graph (the
// engine facts in full, the graph's counters, the add-node form), one Node, one Route, or several with Delete for them
// all. It is the body of AppShell's inspector, under the heading the shell draws from InspectorTitle; the shell docks
// and resizes it, or below the narrow width puts it in a sheet (docs/editor.md#narrow-windows). Every change is a request to ear. An Add command (docs/editor.md#the-palette)
// reaches the add-node form as an intent: its type chosen and the form focused.
import { Button } from "@mcurcio/patchbay/primitives";
import type { Graph, RouteEnds } from "../protocol";
import { subjectTitle, type InspectorSubject, type Selection } from "../selection";
import { wireEnds } from "../wires";
import { AddNodePanel } from "./AddNodePanel";
import { EngineFactsSection } from "./EngineFacts";
import { NodePanel } from "./NodePanel";
import { RoutePanel } from "./RoutePanel";

/** An Add command's type for the add-node form; a new nonce each time, so the same type asked twice acts twice. */
export interface AddIntent {
  type: string;
  nonce: number;
}

function Subject({
  graph,
  subject,
  onSelect,
  onRemove,
  addIntent,
}: {
  graph: Graph;
  subject: InspectorSubject;
  addIntent: AddIntent | null;
  onSelect: (selection: Selection) => void;
  onRemove: (nodes: readonly string[], routes: readonly RouteEnds[]) => void;
}) {
  switch (subject.kind) {
    case "graph":
      return (
        <>
          <EngineFactsSection graph={graph} />
          <AddNodePanel
            types={graph.types}
            names={graph.nodes.map((node) => node.name)}
            features={graph.features ?? []}
            intent={addIntent}
          />
        </>
      );
    case "node":
      return (
        <NodePanel
          key={subject.node.name}
          node={subject.node}
          features={graph.features ?? []}
          onRemove={(name) => onRemove([name], [])}
        />
      );
    case "route":
      return <RoutePanel route={subject.route} onSelect={onSelect} onRemove={(route) => onRemove([], [route])} />;
    case "several":
      return (
        <section className="inspector-section">
          <ul className="inspector-list">
            {subject.nodes.map((node) => (
              <li key={`node ${node.name}`}>{node.name}</li>
            ))}
            {subject.routes.map((route) => {
              const ends = wireEnds(route);
              return (
                <li key={`route ${ends}`} className="port-ref">
                  {ends}
                </li>
              );
            })}
          </ul>
          <Button
            tone="danger"
            className="inspector-delete"
            onClick={() =>
              onRemove(
                subject.nodes.map((node) => node.name),
                subject.routes,
              )
            }
          >
            Delete all
          </Button>
        </section>
      );
  }
}

export function Inspector({
  graph,
  subject,
  onSelect,
  onRemove,
  addIntent,
}: {
  graph: Graph;
  subject: InspectorSubject;
  addIntent: AddIntent | null;
  onSelect: (selection: Selection) => void;
  onRemove: (nodes: readonly string[], routes: readonly RouteEnds[]) => void;
}) {
  // The body under the shell's one header row (InspectorTitle): a Panel's body when docked, the sheet's when narrow.
  // The top bar's Inspector button names it in its aria-controls.
  return (
    <div id="inspector" className="inspector-body">
      <Subject graph={graph} subject={subject} onSelect={onSelect} onRemove={onRemove} addIntent={addIntent} />
    </div>
  );
}

/** The inspector's heading, which AppShell draws as its one header row: the subject's title, a node's with its type. */
export function InspectorTitle({ subject }: { subject: InspectorSubject }) {
  if (subject.kind !== "node") return subjectTitle(subject);
  return (
    <>
      {subject.node.name} <span className="ear-node-type">{subject.node.type}</span>
    </>
  );
}
