// The theme's tokens (docs/editor.md#tokens-and-themes): theme.css's three sets (light, the system's dark, the chosen
// dark), every text, state and knobly pair at 4.5:1 in both themes, no colour of its own in editor.css or a
// component; and the switch's logic.
import { describe, expect, it } from "vitest";
import editorCss from "./editor.css?raw";
import themeCss from "./theme.css?raw";
import { applyTheme, defaultTheme, nextTheme, themeChoices, themeTitle } from "./theme";

const components = import.meta.glob("./components/*.tsx", { query: "?raw", import: "default", eager: true }) as Record<
  string,
  string
>;

function stripComments(text: string): string {
  return text.replace(/\/\*[\s\S]*?\*\//g, "");
}

type Block = { media: string | null; selector: string; declarations: Map<string, string> };

/** theme.css's rule sets: a plain rule or one rule inside an @media (the file holds nothing else). */
function blocks(css: string): Block[] {
  const found: Block[] = [];
  const pattern = /(@media[^{]+)\{\s*([^{}@]+)\{([^{}]*)\}|([^{}@]+)\{([^{}]*)\}/g;
  for (const match of stripComments(css).matchAll(pattern)) {
    const media = match[1]?.trim() ?? null;
    const selector = (match[2] ?? match[4]).trim();
    const body = match[3] ?? match[5];
    const declarations = new Map<string, string>();
    for (const declaration of body.split(";")) {
      const colon = declaration.indexOf(":");
      if (colon < 0) continue;
      declarations.set(declaration.slice(0, colon).trim(), declaration.slice(colon + 1).trim());
    }
    found.push({ media, selector, declarations });
  }
  return found;
}

function tokensOf(block: Block): Map<string, string> {
  return new Map([...block.declarations].filter(([name]) => name.startsWith("--")));
}

const parsed = blocks(themeCss);

function block(media: RegExp | null, selector: string): Block {
  const found = parsed.find(
    (candidate) =>
      candidate.selector === selector &&
      (media === null ? candidate.media === null : candidate.media !== null && media.test(candidate.media)),
  );
  if (found === undefined) throw new Error(`theme.css has no ${selector} rule${media === null ? "" : ` in ${media}`}`);
  return found;
}

const lightBlock = () => block(null, ":root");
const systemDarkBlock = () => block(/prefers-color-scheme:\s*dark/, ':root:not([data-theme="light"])');
const chosenDarkBlock = () => block(null, ':root[data-theme="dark"]');

function theme(name: "light" | "dark"): Map<string, string> {
  const light = tokensOf(lightBlock());
  if (name === "light") return light;
  return new Map([...light, ...tokensOf(chosenDarkBlock())]);
}

function resolve(tokens: Map<string, string>, value: string): string {
  const seen = new Set<string>();
  let current = value;
  for (let reference = /^var\((--[\w-]+)\)$/.exec(current); reference !== null; ) {
    const name = reference[1];
    if (seen.has(name)) throw new Error(`a var() cycle through ${name}`);
    seen.add(name);
    const next = tokens.get(name);
    if (next === undefined) throw new Error(`${name} is not defined`);
    current = next;
    reference = /^var\((--[\w-]+)\)$/.exec(current);
  }
  return current;
}

function hex(value: string): [number, number, number] | null {
  const short = /^#([0-9a-f])([0-9a-f])([0-9a-f])$/i.exec(value);
  if (short !== null) return [1, 2, 3].map((i) => parseInt(short[i] + short[i], 16)) as [number, number, number];
  const long = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(value);
  if (long !== null) return [1, 2, 3].map((i) => parseInt(long[i], 16)) as [number, number, number];
  return null;
}

/** WCAG 2's relative luminance of an sRGB colour. */
function luminance([r, g, b]: [number, number, number]): number {
  const channel = (c: number) => {
    const s = c / 255;
    return s <= 0.04045 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b);
}

function ratio(a: [number, number, number], b: [number, number, number]): number {
  const [high, low] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (high + 0.05) / (low + 0.05);
}

/** Each text token and the surfaces it is drawn on. */
const pairs: { fg: string[]; on: string[] }[] = [
  { fg: ["--text", "--muted"], on: ["--surface", "--raised", "--sunken"] },
  { fg: ["--text"], on: ["--warn-soft", "--err-soft"] },
  { fg: ["--accent"], on: ["--surface", "--raised"] },
  { fg: ["--ok", "--warn", "--err", "--quiet", "--off"], on: ["--surface", "--raised", "--sunken"] },
  { fg: ["--warn"], on: ["--warn-soft"] },
  { fg: ["--err"], on: ["--err-soft"] },
  { fg: ["--on-strong"], on: ["--accent", "--err", "--warn"] },
  {
    fg: [
      "--knobly-text",
      "--knobly-directive",
      "--knobly-keyword",
      "--knobly-variable",
      "--knobly-number",
      "--knobly-string",
      "--knobly-comment",
      "--knobly-punctuation",
      "--knobly-error",
    ],
    on: ["--knobly-surface", "--knobly-surface-readonly"],
  },
  { fg: ["--knobly-error"], on: ["--knobly-error-surface"] },
];

describe("the theme's tokens", () => {
  it("has a light set, a system dark set and a chosen dark set", () => {
    expect(tokensOf(lightBlock()).size).toBeGreaterThan(0);
    expect(tokensOf(systemDarkBlock()).size).toBeGreaterThan(0);
    expect(tokensOf(chosenDarkBlock()).size).toBeGreaterThan(0);
  });

  it("writes the dark set the same both times", () => {
    expect(tokensOf(systemDarkBlock())).toEqual(tokensOf(chosenDarkBlock()));
    expect(systemDarkBlock().declarations.get("color-scheme")).toBe("dark");
    expect(chosenDarkBlock().declarations.get("color-scheme")).toBe("dark");
  });

  it("redefines every colour in the dark set and nothing else", () => {
    const light = tokensOf(lightBlock());
    const dark = tokensOf(chosenDarkBlock());
    const unknown = [...dark.keys()].filter((name) => !light.has(name));
    const missing = [...light]
      .filter(([, value]) => !value.startsWith("var(") && (hex(value) !== null || /\brgb\(/.test(value)))
      .map(([name]) => name)
      .filter((name) => !dark.has(name));
    expect({ unknown, missing }).toEqual({ unknown: [], missing: [] });
  });

  it("keeps every text at 4.5:1 on what it is drawn on, in both themes", () => {
    const failures: string[] = [];
    for (const name of ["light", "dark"] as const) {
      const tokens = theme(name);
      for (const pair of pairs) {
        for (const fg of pair.fg) {
          for (const on of pair.on) {
            const fgValue = resolve(tokens, tokens.get(fg) ?? `undefined ${fg}`);
            const onValue = resolve(tokens, tokens.get(on) ?? `undefined ${on}`);
            const fgColour = hex(fgValue);
            const onColour = hex(onValue);
            if (fgColour === null || onColour === null) {
              failures.push(`${name}: ${fg} (${fgValue}) on ${on} (${onValue}) is not two hex colours`);
              continue;
            }
            const measured = ratio(fgColour, onColour);
            if (measured < 4.5) failures.push(`${name}: ${fg} on ${on} is ${measured.toFixed(2)}:1`);
          }
        }
      }
    }
    expect(failures).toEqual([]);
  });

  it("leaves the editor's styles and components no colour of their own", () => {
    const sources: [string, string][] = [
      ["editor.css", stripComments(editorCss)],
      ...Object.entries(components).map(([file, text]): [string, string] => [file, text]),
    ];
    expect(sources.length).toBeGreaterThan(1);
    const patterns = [/#[0-9a-fA-F]{3,8}\b/g, /\b(rgba?|hsla?)\(/g, /(?<![-\w])(white|black)(?![-\w])/g];
    const failures: string[] = [];
    for (const [file, text] of sources) {
      for (const pattern of patterns) {
        for (const match of text.matchAll(pattern)) failures.push(`${file}: ${match[0]}`);
      }
    }
    expect(failures).toEqual([]);
  });
});

describe("the theme switch", () => {
  it("starts at system", () => {
    expect(defaultTheme).toBe("system");
    expect(themeChoices).toEqual(["system", "light", "dark"]);
  });

  it("cycles system, light, dark and back", () => {
    expect(nextTheme("system")).toBe("light");
    expect(nextTheme("light")).toBe("dark");
    expect(nextTheme("dark")).toBe("system");
  });

  it("writes data-theme for light and dark and removes it for system", () => {
    const root = { dataset: {} as DOMStringMap };
    applyTheme(root, "light");
    expect(root.dataset.theme).toBe("light");
    applyTheme(root, "dark");
    expect(root.dataset.theme).toBe("dark");
    applyTheme(root, "system");
    expect("theme" in root.dataset).toBe(false);
  });

  it("says what the theme is and what a click does", () => {
    expect(themeTitle("system")).toBe("Theme: system (follows the system's setting); click for light");
    expect(themeTitle("light")).toBe("Theme: light; click for dark");
    expect(themeTitle("dark")).toBe("Theme: dark; click for system");
  });
});
