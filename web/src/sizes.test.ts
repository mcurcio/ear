// The shell's first sizes (docs/editor.md#the-shell), and that editor.css's own media queries are patchbay's. The
// bounds, the splitters' drag and keys and the narrow and phone layouts are patchbay's AppShell's, tested there.
import { mediaQueries, narrowBelow, phoneBelow, widthOf } from "@mcurcio/patchbay/hooks";
import {
  clampDrawerHeight,
  clampInspectorWidth,
  drawerBounds,
  drawerMaxHeight,
  inspectorBounds,
} from "@mcurcio/patchbay/shell";
import { describe, expect, it } from "vitest";
import editorCss from "./editor.css?raw";
import { drawerInitialHeight, inspectorInitialWidth } from "./sizes";

describe("the first sizes", () => {
  it("are inside AppShell's bounds as docs/editor.md states them", () => {
    expect(inspectorInitialWidth).toBe(320);
    expect(inspectorBounds).toStrictEqual({ min: 280, max: 560 });
    expect(clampInspectorWidth(inspectorInitialWidth)).toBe(inspectorInitialWidth);
    expect(drawerInitialHeight).toBe(320);
    expect(drawerBounds).toStrictEqual({ min: 120, maxShare: 0.7 });
    // A window 460 px tall or more draws the first drawer as asked; a shorter one draws it at 70 %.
    expect(clampDrawerHeight(drawerInitialHeight, 460)).toBe(drawerInitialHeight);
    expect(drawerMaxHeight(100)).toBe(120);
  });
});

describe("editor.css", () => {
  const css = editorCss.replace(/\/\*[\s\S]*?\*\//g, "");

  it("narrows and grows by patchbay's queries, at the widths docs/editor.md states", () => {
    // patchbay's useMediaBreakpoint picks the width class AppShell and App.tsx read.
    expect(narrowBelow).toBe(760);
    expect(phoneBelow).toBe(600);
    expect([widthOf(890), widthOf(759), widthOf(600), widthOf(599)]).toStrictEqual(["wide", "narrow", "narrow", "phone"]);
    expect(css).toContain(`@media ${mediaQueries.phone}`);
    expect(css).not.toMatch(/@media \(max-width: 599px\)/);
    expect(css).toContain(`@media ${mediaQueries.touch}`);
  });
});
