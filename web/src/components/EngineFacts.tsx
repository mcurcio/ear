// The engine facts in two places (docs/editor.md#the-top-bar, docs/editor.md#the-inspector): the top bar's chip (the
// rate and block size, realtime or the "not realtime" badge) with the dropouts badge and the newer-protocol chip; and
// the inspector's Graph subject, the facts in full with the graph's counters (docs/editor.md#counters). The words are
// shell.ts's and counters.ts's.
import { Badge, KeyValue } from "@mcurcio/patchbay/primitives";
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
      <Badge tone="muted" className="engine-chip" title={chip.title}>
        {chip.text} ·{" "}
        {chip.realtime ? (
          "realtime"
        ) : (
          <Badge tone="err" variant="filled">
            not realtime
          </Badge>
        )}
      </Badge>
      <DropoutsBadge badge={badge} />
      {protocol !== undefined && (
        <Badge tone="warn" title={protocol.title}>
          {protocol.text}
        </Badge>
      )}
    </>
  );
}

export function EngineFactsSection({ graph }: { graph: Graph }) {
  const { rows } = useGraphCounters(graph);
  return (
    <section className="inspector-section">
      <h3>Engine</h3>
      <KeyValue
        className="facts"
        rows={engineFacts(graph).map((fact) => ({
          key: fact.label,
          value: fact.value,
          tone: fact.tone,
          title: fact.title,
        }))}
      />
      <CounterTable heading="Since ear started" rows={rows} />
    </section>
  );
}
