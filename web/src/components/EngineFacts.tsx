// The engine facts in two places (docs/editor.md#the-top-bar, docs/editor.md#the-inspector): the top bar's chip (the
// rate and block size, realtime or the "not realtime" badge) with the dropouts badge and the newer-protocol chip; and
// the inspector's Graph subject, the facts in full with the graph's counters (docs/editor.md#counters). The words are
// shell.ts's and counters.ts's.
import type { Graph } from "../protocol";
import { engineChip, engineFacts, protocolChip } from "../shell";
import { CounterTable, DropoutsBadge } from "./Counters";
import { useGraphCounters } from "./hooks";

export function GraphChips({ graph }: { graph: Graph }) {
  const chip = engineChip(graph);
  const { badge } = useGraphCounters(graph);
  const protocol = protocolChip(graph);
  return (
    <>
      <span className="chip engine-chip" title={chip.title}>
        {chip.text} ·{" "}
        {chip.realtime ? (
          <span className="engine-realtime">realtime</span>
        ) : (
          <span className="badge-not-realtime">not realtime</span>
        )}
      </span>
      <DropoutsBadge badge={badge} />
      {protocol !== undefined && (
        <span className="chip chip-protocol tone-warn" title={protocol.title}>
          {protocol.text}
        </span>
      )}
    </>
  );
}

export function EngineFactsSection({ graph }: { graph: Graph }) {
  const { rows } = useGraphCounters(graph);
  return (
    <section className="inspector-section">
      <h3>Engine</h3>
      <dl className="facts">
        {engineFacts(graph).map((fact) => (
          <div key={fact.key} className={fact.tone !== undefined ? `tone-${fact.tone}` : undefined}>
            <dt title={fact.title}>{fact.label}</dt>
            <dd>{fact.value}</dd>
          </div>
        ))}
      </dl>
      <CounterTable heading="Since ear started" rows={rows} />
    </section>
  );
}
