// The shell's sizes (docs/editor.md#the-shell): the bounds, and that editor.css holds the same bounds and breakpoint.
// A splitter's drag and keys are patchbay's Splitter's, tested there.
import { describe, expect, it } from "vitest";
import editorCss from "./editor.css?raw";
import {
  clamp,
  drawerBounds,
  drawerHeight,
  inspectorBounds,
  inspectorWidth,
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
    expect(narrowWidth).toBe(760);
    expect(narrowQuery).toBe("(max-width: 759px)");
    expect(css).toContain(`@media ${narrowQuery}`);
  });
});
