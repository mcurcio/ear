// The theme switch (docs/editor.md#tokens-and-themes): one button that cycles system, light and dark. Its word is
// shown beside the icon, and its label always names the choice, so it is never just a glyph; the icon is SVG, since a
// font may lack a ◐ character. The choice is held by App and the switch sits in the top bar
// (docs/editor.md#the-top-bar).
import { nextTheme, themeTitle, type ThemeChoice } from "../theme";

export function ThemeSwitch({ choice, onChoose }: { choice: ThemeChoice; onChoose: (next: ThemeChoice) => void }) {
  return (
    <button
      type="button"
      className="theme-switch"
      title={themeTitle(choice)}
      aria-label={`Theme: ${choice}`}
      onClick={() => onChoose(nextTheme(choice))}
    >
      <svg className="theme-icon" viewBox="0 0 16 16" width="14" height="14" aria-hidden="true">
        <circle cx="8" cy="8" r="6.25" fill="none" stroke="currentColor" strokeWidth="1.5" />
        <path d="M8 1.75a6.25 6.25 0 0 1 0 12.5z" fill="currentColor" />
      </svg>
      <span className="theme-word">{choice}</span>
    </button>
  );
}
