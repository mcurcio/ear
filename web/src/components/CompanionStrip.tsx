// A node's companions (docs/editor.md#companions): the box's strip of badges, each its kind's word, a dot filled when
// ready and hollow when not, and a warning mark while its counter rose in the last minute; and the node panel's list
// of each one's detail. What each shows is companions.ts's; every state is in text, never in colour alone.
import type { CompanionBadge } from "../companions";
import { WarnMark } from "./WarnMark";

function Dot({ badge }: { badge: CompanionBadge }) {
  return (
    <>
      <span className="companion-dot" aria-hidden="true">
        {badge.ready ? "●" : "○"}
      </span>
      <span className="visually-hidden">{badge.readyText}</span>
    </>
  );
}

function Mark({ badge }: { badge: CompanionBadge }) {
  return badge.warning ? <WarnMark text={badge.warningText} /> : null;
}

/** The strip along the box's bottom edge; nothing when the node has no companions to draw. */
export function CompanionStrip({ badges }: { badges: CompanionBadge[] }) {
  if (badges.length === 0) return null;
  return (
    <ul className="companion-strip" aria-label="companions">
      {badges.map((badge) => (
        <li
          key={badge.key}
          className={`companion-badge${badge.ready ? "" : " companion-not-ready"}`}
          title={badge.title}
        >
          {badge.label} <Dot badge={badge} />
          <Mark badge={badge} />
        </li>
      ))}
    </ul>
  );
}

/** The node panel's list: each companion's detail in words, with its dot and mark. */
export function CompanionList({ badges }: { badges: CompanionBadge[] }) {
  if (badges.length === 0) return null;
  return (
    <div className="companion-list">
      <h3>Companions</h3>
      <ul>
        {badges.map((badge) => (
          <li key={badge.key}>
            <span className="companion-dot" aria-hidden="true">
              {badge.ready ? "●" : "○"}
            </span>{" "}
            {badge.title}
            {badge.warning && (
              <span className="warn-mark" aria-hidden="true">
                !
              </span>
            )}
          </li>
        ))}
      </ul>
    </div>
  );
}
