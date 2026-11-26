// A node's companions (docs/editor.md#companions): the box's strip of badges in NodeBox's footer, each patchbay's Badge
// with its kind's word, in the ok tone with a filled dot when ready and the off tone with a hollow one when not, and a
// warning mark while its counter rose in the last minute; and the node panel's list of each one's detail. What each
// shows is companions.ts's; every state is in text, never in colour alone.
import type { CompanionBadge } from "../companions";
import { Badge, WarnMark } from "@mcurcio/patchbay/primitives";

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
  return badge.warning ? <WarnMark label={badge.warningText} /> : null;
}

/** The mark alone, where its words are already in the text beside it (the detail carries the last minute's count). */
function Glyph() {
  return (
    <span aria-hidden="true">
      <WarnMark label="" />
    </span>
  );
}

/** The strip in the box's footer; nothing when the node has no companions to draw. */
export function CompanionStrip({ badges }: { badges: CompanionBadge[] }) {
  if (badges.length === 0) return null;
  return (
    <ul className="companion-strip" aria-label="companions">
      {badges.map((badge) => (
        <li key={badge.key}>
          <Badge tone={badge.ready ? "ok" : "off"} className="companion-badge" title={badge.title}>
            {badge.label} <Dot badge={badge} />
            <Mark badge={badge} />
          </Badge>
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
            {badge.warning && <Glyph />}
          </li>
        ))}
      </ul>
    </div>
  );
}
