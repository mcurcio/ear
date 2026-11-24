// The top bar (docs/editor.md#the-top-bar): what is about the whole graph and never about a selection. The wordmark and
// the connection pill; the engine chip, the dropouts badge and the newer-protocol chip; then the palette button with
// its key (docs/editor.md#the-palette), the theme switch, the
// ear.conf toggle with the drawer's status words, and below the narrow width the inspector's button. The words are
// shell.ts's and drawer.ts's.
import { drawerChip, type DrawerWord } from "../drawer";
import type { Graph } from "../protocol";
import { connectionPill, nextPillChange } from "../shell";
import type { ThemeChoice } from "../theme";
import { GraphChips } from "./EngineFacts";
import { useConnectionView, useRecentNow } from "./hooks";
import { ThemeSwitch } from "./ThemeSwitch";

/** The connection pill, drawn again each time its countdown's seconds drop. */
function ConnectionPill() {
  const view = useConnectionView();
  const now = useRecentNow((t) => nextPillChange(view, t));
  const pill = connectionPill(view, now);
  return (
    <span className={`pill tone-${pill.tone}`} role="status" title={pill.title}>
      <span className="pill-dot" aria-hidden="true" />
      {pill.word}
    </span>
  );
}

export function TopBar({
  graph,
  theme,
  onTheme,
  drawer,
  narrow,
  sheetOpen,
  onToggleSheet,
  onPalette,
  paletteLabel = "⌘K",
}: {
  graph: Graph | null;
  theme: ThemeChoice;
  onTheme: (next: ThemeChoice) => void;
  /** null: ear sent no graph file, so there is no drawer to toggle. */
  drawer: { open: boolean; status: DrawerWord[]; onToggle: () => void } | null;
  narrow: boolean;
  sheetOpen: boolean;
  onToggleSheet: () => void;
  /** The command palette's button is drawn only when given. */
  onPalette?: () => void;
  /** The palette's key as the platform writes it: "⌘K" or "Ctrl+K". */
  paletteLabel?: string;
}) {
  const chip = drawer === null ? undefined : drawerChip(drawer.status);
  return (
    <header className="topbar">
      <div className="topbar-start">
        <span className="wordmark">EAR</span>
        <ConnectionPill />
      </div>
      <div className="topbar-facts">{graph !== null && <GraphChips graph={graph} />}</div>
      <div className="topbar-end">
        {onPalette !== undefined && (
          <button
            type="button"
            className="topbar-palette"
            aria-label="Command palette"
            title={`Command palette (${paletteLabel})`}
            aria-keyshortcuts={paletteLabel.startsWith("⌘") ? "Meta+K" : "Control+K"}
            onClick={onPalette}
          >
            {paletteLabel}
          </button>
        )}
        <ThemeSwitch choice={theme} onChoose={onTheme} />
        {drawer !== null && (
          <button
            type="button"
            className="topbar-drawer"
            aria-expanded={drawer.open}
            aria-controls="drawer-body"
            title={drawer.open ? "Hide ear.conf" : "Show ear.conf"}
            onClick={drawer.onToggle}
          >
            <span aria-hidden="true">▤</span> ear.conf
            {chip !== undefined && <span className={`chip tone-${chip.tone}`}>{chip.text}</span>}
          </button>
        )}
        {narrow && (
          <button
            type="button"
            className="topbar-inspector"
            aria-expanded={sheetOpen}
            aria-controls="inspector"
            onClick={onToggleSheet}
          >
            Inspector
          </button>
        )}
      </div>
    </header>
  );
}
