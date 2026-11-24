// The theme switch (docs/editor.md#tokens-and-themes): one button that cycles system, light and dark, its word always
// shown so it is never just a glyph. The choice is held by App.
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
      <span aria-hidden="true">◐</span> {choice}
    </button>
  );
}
