// The editor's tokens (docs/editor.md#tokens-and-themes): every var() in editor.css and the editor's sources names a
// token in patchbay's tokens.css or one of ear's own variables, and neither editor.css nor a component holds a colour
// of its own. patchbay's tokens test holds its sets, the two dark copies equal and every text pair at 4.5:1. And the
// theme's first paint: index.html inlines patchbay's themeScript for ear's key.
import { themeScript } from "@mcurcio/patchbay/hooks";
import tokensCss from "@mcurcio/patchbay/tokens.css?raw";
import { describe, expect, it } from "vitest";
import editorCss from "./editor.css?raw";
import indexHtml from "../index.html?raw";
import { themeKey } from "./prefs";

const components = import.meta.glob("./components/*.tsx", { query: "?raw", import: "default", eager: true }) as Record<
  string,
  string
>;

/** Every source of the editor's but its tests, as text. */
const sources = import.meta.glob(["./**/*.ts", "./**/*.tsx", "!./**/*.test.ts", "!./**/*.test.tsx"], {
  query: "?raw",
  import: "default",
  eager: true,
}) as Record<string, string>;

function stripComments(text: string): string {
  return text.replace(/\/\*[\s\S]*?\*\//g, "");
}

/**
 * ear's own variables (docs/editor.md#tokens-and-themes): never a colour, radius, shadow, duration or type size of
 * their own, only a value one rule or one component sets, or a token picked per state. Each is listed with where it is
 * set.
 */
const earVariables = new Map<string, string>([
  ["--state", "editor.css: a node's state class picks its tone"],
  ["--toast-tone", "editor.css: a toast's kind picks its tone"],
  ["--wire-colour", "editor.css: a wire's status class picks its tone"],
  ["--wire-width", "LevelEdge.tsx: the wire's width from its level"],
  ["--meter-mid-at", "Meter.tsx: where the mid zone starts"],
  ["--meter-hot-at", "Meter.tsx: where the hot zone starts"],
  ["--meter-rms-at", "Meter.tsx: the bar's level"],
  ["--meter-peak-at", "Meter.tsx: the peak tick's level"],
  ["--inspector-width", "Inspector.tsx: the inspector's width"],
  ["--drawer-height", "Drawer.tsx: the drawer's height"],
  ["--gutter-digits", "KnoblyText.tsx: the gutter's width in digits"],
]);

/** The custom properties a stylesheet declares. */
function declared(css: string): Set<string> {
  return new Set([...stripComments(css).matchAll(/(?<![\w-])(--[\w-]+)\s*:/g)].map((match) => match[1]));
}

/** The custom properties a text reads with var(). */
function used(text: string): Set<string> {
  return new Set([...text.matchAll(/var\(\s*(--[\w-]+)/g)].map((match) => match[1]));
}

describe("the editor's tokens", () => {
  const tokens = declared(tokensCss);

  it("reads patchbay's tokens", () => {
    for (const name of ["--pb-surface", "--pb-text", "--pb-code-size", "--pb-font-ui", "--pb-text-md"]) {
      expect(tokens.has(name), name).toBe(true);
    }
  });

  it("names only patchbay's tokens and ear's own variables in every var()", () => {
    const texts: [string, string][] = [["editor.css", stripComments(editorCss)], ...Object.entries(sources)];
    expect(texts.length).toBeGreaterThan(10);
    const unknown: string[] = [];
    for (const [file, text] of texts) {
      for (const name of used(text)) {
        if (!tokens.has(name) && !earVariables.has(name)) unknown.push(`${file}: ${name}`);
      }
    }
    expect(unknown).toEqual([]);
  });

  it("sets each of ear's own variables somewhere, and none in patchbay's names", () => {
    const set = declared(editorCss);
    const everywhere = [editorCss, ...Object.values(sources)].join("\n");
    for (const name of earVariables.keys()) {
      expect(name.startsWith("--pb-"), name).toBe(false);
      expect(tokens.has(name), name).toBe(false);
      expect(set.has(name) || everywhere.includes(`"${name}"`), `${name} is never set`).toBe(true);
    }
  });

  it("declares in editor.css only ear's own variables and patchbay's tokens", () => {
    const unknown = [...declared(editorCss)].filter((name) => !tokens.has(name) && !earVariables.has(name));
    expect(unknown).toEqual([]);
  });

  it("leaves the editor's styles and components no colour of their own", () => {
    const texts: [string, string][] = [
      ["editor.css", stripComments(editorCss)],
      ...Object.entries(components).map(([file, text]): [string, string] => [file, text]),
    ];
    expect(texts.length).toBeGreaterThan(1);
    const patterns = [/#[0-9a-fA-F]{3,8}\b/g, /\b(rgba?|hsla?)\(/g, /(?<![-\w])(white|black)(?![-\w])/g];
    const failures: string[] = [];
    for (const [file, text] of texts) {
      for (const pattern of patterns) {
        for (const match of text.matchAll(pattern)) failures.push(`${file}: ${match[0]}`);
      }
    }
    expect(failures).toEqual([]);
  });
});

describe("the theme's first paint", () => {
  it("inlines patchbay's themeScript for ear's key in index.html, before any stylesheet or module", () => {
    const script = themeScript(themeKey());
    const inline = /<script>([\s\S]*?)<\/script>/.exec(indexHtml);
    expect(inline?.[1]).toBe(script);
    expect(indexHtml.indexOf(script)).toBeLessThan(indexHtml.indexOf('<script type="module"'));
  });
});
