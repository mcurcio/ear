// The diagnostics counters (docs/editor.md#counters): a node box's mark beside its state word, the graph's dropouts
// badge beside "not realtime", and the panel's tables of every total, an unknown counter by its name and number. What
// each shows is counters.ts's; every mark has its words, never colour alone.
import type { CounterMark, CounterRow, DropoutBadge } from "../counters";
import { WarnMark } from "./WarnMark";

/** The box's mark: nothing unless a loss counter of the node rose in the last minute. */
export function NodeCounterMark({ mark }: { mark: CounterMark }) {
  if (!mark.warning) return null;
  return (
    <span className="counter-mark" title={mark.title}>
      <WarnMark text={mark.warningText} />
    </span>
  );
}

/** The graph's badge: nothing unless the driver cut a cycle in the last minute. */
export function DropoutsBadge({ badge }: { badge: DropoutBadge | undefined }) {
  if (badge === undefined) return null;
  return (
    <span className="badge-dropouts" title={badge.title}>
      <span className="warn-mark" aria-hidden="true">
        !
      </span>{" "}
      {badge.text}
    </span>
  );
}

/** A panel's table of totals under its heading; nothing without counters (an ear without the feature). */
export function CounterTable({ heading, rows }: { heading: string; rows: CounterRow[] }) {
  if (rows.length === 0) return null;
  return (
    <div className="counter-table">
      <h3>{heading}</h3>
      <table>
        <tbody>
          {rows.map((row) => (
            <tr key={row.key} className={row.known ? undefined : "counter-unknown"}>
              <th scope="row" title={row.title}>
                {row.label}
              </th>
              <td>{row.text}</td>
              <td>
                {row.warning && (
                  <>
                    <span className="warn-mark" aria-hidden="true">
                      !
                    </span>{" "}
                    <span className="counter-recent">{row.warningText}</span>
                  </>
                )}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
