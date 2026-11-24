// The shell's sizes (docs/editor.md#the-shell): the bounds, a splitter's drag and keys, and that editor.css holds the
// same bounds and breakpoint.
import { describe, expect, it } from "vitest";
import editorCss from "./editor.css?raw";
import {
  clamp,
  dragged,
  drawerBounds,
  drawerHeight,
  inspectorBounds,
  inspectorWidth,
  keyStep,
  keyed,
  narrowQuery,
  narrowWidth,
} from "./sizes";

describe("the bounds", () => {
  it("keep the inspector between 280 and 560 px", () => {
    expect(inspectorWidth.initial).toBe(320);
    expect(inspectorBounds).toStrictEqual({ min: 280, max: 560 });
    expect(clamp(100, inspectorBounds)).toBe(280);
    expect(clamp(320.4, inspectorBounds)).toBe(320);
    expect(clamp(9000, inspectorBounds)).toBe(560);
  });

  it("keep the drawer between 120 px and 70 % of the window, the max never below the min", () => {
    expect(drawerHeight.initial).toBe(280);
    expect(drawerBounds(1000)).toStrictEqual({ min: 120, max: 700 });
    expect(drawerBounds(100)).toStrictEqual({ min: 120, max: 120 });
  });
});

describe("a splitter", () => {
  it("grows the inspector as its left edge is dragged left, clamped at both ends", () => {
    expect(dragged(320, -40, -1, inspectorBounds)).toBe(360);
    expect(dragged(320, 40, -1, inspectorBounds)).toBe(280);
    expect(dragged(320, -1000, -1, inspectorBounds)).toBe(560);
    expect(dragged(320, 1000, -1, inspectorBounds)).toBe(280);
  });

  it("moves by keys: the grow arrow, its opposite, Home and End", () => {
    expect(keyStep).toBe(16);
    expect(keyed(320, "ArrowLeft", "ArrowLeft", inspectorBounds)).toBe(336);
    expect(keyed(320, "ArrowRight", "ArrowLeft", inspectorBounds)).toBe(304);
    expect(keyed(550, "ArrowLeft", "ArrowLeft", inspectorBounds)).toBe(560);
    expect(keyed(320, "Home", "ArrowLeft", inspectorBounds)).toBe(280);
    expect(keyed(320, "End", "ArrowLeft", inspectorBounds)).toBe(560);
    expect(keyed(320, "a", "ArrowLeft", inspectorBounds)).toBeUndefined();
    expect(keyed(320, "ArrowUp", "ArrowLeft", inspectorBounds)).toBeUndefined();

    const drawer = drawerBounds(1000);
    expect(keyed(280, "ArrowUp", "ArrowUp", drawer)).toBe(296);
    expect(keyed(280, "ArrowDown", "ArrowUp", drawer)).toBe(264);
    expect(keyed(130, "ArrowDown", "ArrowUp", drawer)).toBe(120);
    expect(keyed(280, "Home", "ArrowUp", drawer)).toBe(120);
    expect(keyed(280, "End", "ArrowUp", drawer)).toBe(700);
    expect(keyed(280, "ArrowLeft", "ArrowUp", drawer)).toBeUndefined();
  });
});

describe("editor.css", () => {
  const css = editorCss.replace(/\/\*[\s\S]*?\*\//g, "");
  const declaration = (selector: string, property: string): string => {
    const rule = new RegExp(`(?:^|\\})\\s*${selector.replace(/[.]/g, "\\.")}\\s*\\{([^}]*)\\}`).exec(css);
    if (rule === null) throw new Error(`editor.css has no ${selector} rule`);
    const value = new RegExp(`(?:^|;)\\s*${property}\\s*:\\s*([^;]+)`).exec(rule[1]!);
    if (value === null) throw new Error(`editor.css's ${selector} rule has no ${property}`);
    return value[1]!.trim();
  };

  it("holds the inspector's and the drawer's bounds", () => {
    expect(declaration(".inspector", "width")).toBe(
      `clamp(${inspectorBounds.min}px, var(--inspector-width), ${inspectorBounds.max}px)`,
    );
    expect(declaration(".drawer-body", "height")).toBe(
      `clamp(${drawerHeight.min}px, var(--drawer-height), ${drawerHeight.maxShare * 100}vh)`,
    );
  });

  it("makes the inspector a sheet below the narrow width", () => {
    expect(narrowWidth).toBe(900);
    expect(narrowQuery).toBe("(max-width: 899px)");
    expect(css).toContain(`@media ${narrowQuery}`);
  });
});
