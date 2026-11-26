// The top bar (docs/editor.md#the-top-bar): what is about the whole graph and never about a selection, as the slots of
// patchbay's AppShell. The wordmark (the brand, dropped at phone width) and the connection pill (the status); the
// engine chip, the dropouts badge and the newer-protocol chip (the start); then the palette button with its key
// (docs/editor.md#the-palette), patchbay's theme switch, the ear.conf toggle with the drawer's status words, and below
// the narrow width the inspector's button (the end). The three buttons are patchbay's Button, so they stand as tall
// as the theme switch beside them. The words are shell.ts's and drawer.ts's.
import type { ThemeChoice } from "@mcurcio/patchbay/hooks";
import { Badge, Button, Pill, ThemeSwitch } from "@mcurcio/patchbay/primitives";
import type { AppShellTopBar } from "@mcurcio/patchbay/shell";
import { drawerChip, type DrawerWord } from "../drawer";
import type { Graph } from "../protocol";
import { connectionPill, nextPillChange } from "../shell";
import { GraphChips } from "./EngineFacts";
import { useConnectionView, useRecentNow } from "./hooks";

/** The connection pill, drawn again each time its countdown's seconds drop; a status region, so a change is read. */
function ConnectionPill() {
  const view = useConnectionView();
  const now = useRecentNow((t) => nextPillChange(view, t));
  const pill = connectionPill(view, now);
  return (
    <span className="connection-pill" role="status">
      <Pill tone={pill.tone} label={pill.word} title={pill.title} />
    </span>
  );
}

export interface TopBarProps {
  graph: Graph | null;
  theme: ThemeChoice;
  onTheme: (next: ThemeChoice) => void;
  /** null: ear sent no graph file, so there is no drawer to toggle. */
  drawer: { open: boolean; status: DrawerWord[]; onToggle: () => void } | null;
  /** patchbay's width class: the inspector's button below the narrow width, no wordmark below the phone width. */
  width: "wide" | "narrow" | "phone";
  sheetOpen: boolean;
  onToggleSheet: () => void;
  onPalette: () => void;
  /** The palette's key as the platform writes it: "⌘K" or "Ctrl+K". */
  paletteLabel: string;
}

/** The top bar's slots for AppShell. */
export function topBar({
  graph,
  theme,
  onTheme,
  drawer,
  width,
  sheetOpen,
  onToggleSheet,
  onPalette,
  paletteLabel,
}: TopBarProps): AppShellTopBar {
  const chip = drawer === null ? undefined : drawerChip(drawer.status);
  return {
    brand: width === "phone" ? undefined : <span className="wordmark">EAR</span>,
    status: <ConnectionPill />,
    start: <div className="topbar-facts">{graph !== null && <GraphChips graph={graph} />}</div>,
    end: (
      <>
        <Button
          className="topbar-palette"
          aria-label="Command palette"
          title={`Command palette (${paletteLabel})`}
          aria-keyshortcuts={paletteLabel.startsWith("⌘") ? "Meta+K" : "Control+K"}
          onClick={onPalette}
        >
          {/* On a touch screen, with no keyboard to press it on, the key gives way to a search icon (editor.css). */}
          <svg className="palette-icon" viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">
            <circle cx="7" cy="7" r="4.75" fill="none" stroke="currentColor" strokeWidth="1.5" />
            <path d="M10.5 10.5 14 14" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
          </svg>
          <span className="palette-key">{paletteLabel}</span>
        </Button>
        <ThemeSwitch choice={theme} onChoose={onTheme} />
        {drawer !== null && (
          <Button
            className="topbar-drawer"
            aria-expanded={drawer.open}
            aria-controls="drawer-body"
            title={drawer.open ? "Hide ear.conf" : "Show ear.conf"}
            onClick={drawer.onToggle}
          >
            <span className="topbar-drawer-glyph" aria-hidden="true">
              ▤
            </span>
            ear.conf
            {chip !== undefined && <Badge tone={chip.tone}>{chip.text}</Badge>}
          </Button>
        )}
        {width !== "wide" && (
          <Button
            className="topbar-inspector"
            aria-expanded={sheetOpen}
            aria-controls="inspector"
            onClick={onToggleSheet}
          >
            Inspector
          </Button>
        )}
      </>
    ),
  };
}
