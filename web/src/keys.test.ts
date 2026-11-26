// The editor's key bindings (docs/editor.md#shortcuts), run through patchbay's findShortcut as useShortcuts runs them:
// Mod by platform, the three chords in a text field, single keys and Escape only outside one, and only the palette's
// toggle while it is open. How keys are read in general is patchbay's, tested there.
import { findShortcut, keyLabel, type Platform } from "@mcurcio/patchbay/hooks";
import { describe, expect, it } from "vitest";
import { shortcutKeys, shortcutLines, shortcutTable, type ShortcutAction } from "./keys";

/** A stand-in for the element a key was pressed in: a text field or not. */
function target(tagName: string): EventTarget {
  return { tagName, closest: () => null, isContentEditable: false } as unknown as EventTarget;
}

interface Press {
  key: string;
  mods?: Partial<Pick<KeyboardEvent, "metaKey" | "ctrlKey" | "altKey" | "shiftKey">>;
  in?: string;
}

function keydown({ key, mods = {}, in: tag = "DIV" }: Press): KeyboardEvent {
  const code = /^[a-z]$/i.test(key) ? `Key${key.toUpperCase()}` : "";
  return {
    key,
    code,
    metaKey: false,
    ctrlKey: false,
    altKey: false,
    shiftKey: false,
    repeat: false,
    isComposing: false,
    defaultPrevented: false,
    target: target(tag),
    ...mods,
  } as unknown as KeyboardEvent;
}

/** The action a press runs, by the editor's table. */
function actionOf(press: Press, platform: Platform, state = { palette: false }): ShortcutAction | undefined {
  let ran: ShortcutAction | undefined;
  const table = shortcutTable((action) => (ran = action), state);
  findShortcut(keydown(press), table, platform)?.run(keydown(press));
  return ran;
}

describe("the shortcuts", () => {
  it("Mod is Command on Apple platforms and Ctrl elsewhere", () => {
    expect(actionOf({ key: "k", mods: { metaKey: true } }, "apple")).toBe("palette");
    expect(actionOf({ key: "k", mods: { metaKey: true } }, "other")).toBeUndefined();
    expect(actionOf({ key: "k", mods: { ctrlKey: true } }, "other")).toBe("palette");
    expect(actionOf({ key: "k", mods: { ctrlKey: true } }, "apple")).toBeUndefined();
    expect(actionOf({ key: "e", mods: { ctrlKey: true } }, "other")).toBe("drawer");
    expect(actionOf({ key: "s", mods: { metaKey: true } }, "apple")).toBe("save");
    expect(actionOf({ key: "k", mods: { metaKey: true, altKey: true } }, "apple")).toBeUndefined();
  });

  it("chords act in a text field, single keys and Escape do not", () => {
    expect(actionOf({ key: "k", mods: { ctrlKey: true }, in: "TEXTAREA" }, "other")).toBe("palette");
    expect(actionOf({ key: "e", mods: { ctrlKey: true }, in: "INPUT" }, "other")).toBe("drawer");
    expect(actionOf({ key: "s", mods: { ctrlKey: true }, in: "TEXTAREA" }, "other")).toBe("save");
    for (const key of ["f", "l", "m", "a", "?", "Escape"]) {
      expect(actionOf({ key, mods: { shiftKey: key === "?" }, in: "TEXTAREA" }, "other")).toBeUndefined();
    }
    expect(actionOf({ key: "Escape" }, "other")).toBe("escape");
  });

  it("single keys need no modifier but the Shift a character needs", () => {
    expect(["f", "l", "m", "a"].map((key) => actionOf({ key }, "other"))).toStrictEqual([
      "fit",
      "tidy",
      "minimap",
      "add",
    ]);
    expect(actionOf({ key: "f", mods: { ctrlKey: true } }, "other")).toBeUndefined();
    expect(actionOf({ key: "f", mods: { metaKey: true } }, "apple")).toBeUndefined();
    expect(actionOf({ key: "f", mods: { altKey: true } }, "other")).toBeUndefined();
    expect(actionOf({ key: "?", mods: { shiftKey: true } }, "other")).toBe("shortcuts");
    expect(actionOf({ key: "Delete" }, "other")).toBeUndefined();
    expect(actionOf({ key: "Backspace" }, "other")).toBeUndefined();
  });

  it("only the palette's toggle acts while the palette is open", () => {
    const open = { palette: true };
    expect(actionOf({ key: "k", mods: { metaKey: true }, in: "INPUT" }, "apple", open)).toBe("palette");
    for (const press of [{ key: "s", mods: { metaKey: true } }, { key: "f" }, { key: "Escape" }]) {
      expect(actionOf(press, "apple", open)).toBeUndefined();
    }
  });

  it("the sheet lists every binding, and React Flow's Delete", () => {
    const listed = new Set(shortcutLines.map((line) => line.keys));
    for (const keys of Object.values(shortcutKeys)) expect(listed.has(keys)).toBe(true);
    expect(shortcutLines.some((line) => Array.isArray(line.keys) && line.keys.includes("Delete"))).toBe(true);
    for (const line of shortcutLines) expect(line.label).not.toBe("");
    expect(keyLabel(shortcutKeys.palette, { platform: "apple" })).toBe("⌘K");
    expect(keyLabel(shortcutKeys.palette, { platform: "other" })).toBe("Ctrl+K");
  });
});
