// The theme (docs/editor.md#tokens-and-themes): system, light or dark, written to data-theme on <html>. System removes
// the attribute, so theme.css's prefers-color-scheme rule decides. The switch sits in the top bar
// (docs/editor.md#the-top-bar). The choice is kept per browser by prefs.ts (docs/editor.md#what-the-browser-keeps).

export type ThemeChoice = "system" | "light" | "dark";

/** The switch's cycle order. */
export const themeChoices: readonly ThemeChoice[] = ["system", "light", "dark"];

/** The editor follows the system until the person chooses. */
export const defaultTheme: ThemeChoice = "system";

export function nextTheme(choice: ThemeChoice): ThemeChoice {
  return themeChoices[(themeChoices.indexOf(choice) + 1) % themeChoices.length];
}

/** The switch's tooltip: what the theme is and what a click does. */
export function themeTitle(choice: ThemeChoice): string {
  switch (choice) {
    case "system":
      return "Theme: system (follows the system's setting); click for light";
    case "light":
      return "Theme: light; click for dark";
    case "dark":
      return "Theme: dark; click for system";
  }
}

/** Writes the choice onto the root it is given (the page's <html>), so this module touches no DOM global. */
export function applyTheme(root: { dataset: DOMStringMap }, choice: ThemeChoice): void {
  if (choice === "system") delete root.dataset.theme;
  else root.dataset.theme = choice;
}
