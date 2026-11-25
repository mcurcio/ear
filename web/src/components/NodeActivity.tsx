// The node panel's status list, its diagnostics and its activity (activity.ts, docs/editor.md#status-events-and-diagnostics):
// every status field in words, every diagnostic the node was told with its count, and the events and diagnostics since
// this page connected, newest first. None is drawn when there is nothing to list.
import { diagnosticLabel, diagnosticTitle, statusRows } from "../activity";
import type { GraphNode, TypeSchema } from "../protocol";
import { useActivity } from "./hooks";

export function NodeStatus({ node, schema }: { node: GraphNode; schema?: TypeSchema | undefined }) {
  const rows = statusRows(node, schema);
  if (rows.length === 0) return null;
  return (
    <section className="node-status" aria-label="status">
      <h3>Status</h3>
      <dl>
        {rows.map((row) => (
          <div key={row.name} title={row.help}>
            <dt>{row.name}</dt>
            <dd>{row.text}</dd>
          </div>
        ))}
      </dl>
    </section>
  );
}

export function NodeDiagnostics({ node }: { node: GraphNode }) {
  const kept = node.diagnostics ?? [];
  if (kept.length === 0) return null;
  return (
    <section className="node-diagnostics" aria-label="diagnostics">
      <h3>Diagnostics</h3>
      <ul>
        {kept.map((diagnostic) => (
          <li key={`${diagnostic.code}\n${diagnostic.subject}`} className={diagnostic.active ? "active" : undefined}>
            <strong>{diagnosticLabel(diagnostic)}</strong>
            {diagnostic.active && <span className="visually-hidden"> (active)</span>} {diagnosticTitle(diagnostic)}
          </li>
        ))}
      </ul>
    </section>
  );
}

export function NodeActivity({ node }: { node: string }) {
  const entries = useActivity(node);
  if (entries.length === 0) return null;
  return (
    <section className="node-activity" aria-label="activity">
      <h3>Activity</h3>
      <ol reversed>
        {[...entries].reverse().map((entry, i) => (
          <li key={entries.length - i} className={`activity-${entry.severity}`}>
            <time dateTime={entry.t}>{entry.t.slice(11, 19)}</time> {entry.msg}
          </li>
        ))}
      </ol>
    </section>
  );
}
