// The shortcuts (docs/editor.md#shortcuts): Mod by platform, chords in a text field, single keys outside one.
import { describe, expect, it } from "vitest";
import { isMac, isTextTarget, keyLabel, rowKeys, shortcutOf, shortcutRows, type KeyLike } from "./keys";

function key(name: string, mods: Partial<Omit<KeyLike, "key">> = {}): KeyLike {
  return { key: name, metaKey: false, ctrlKey: false, altKey: false, shiftKey: false, ...mods };
}

describe("the shortcuts", () => {
  it("Mod is Command on a Mac and Ctrl elsewhere", () => {
    expect(shortcutOf(key("k", { metaKey: true }), true, false)).toBe("palette");
    expect(shortcutOf(key("k", { metaKey: true }), false, false)).toBeUndefined();
    expect(shortcutOf(key("k", { ctrlKey: true }), false, false)).toBe("palette");
    expect(shortcutOf(key("k", { ctrlKey: true }), true, false)).toBeUndefined();
    expect(shortcutOf(key("e", { ctrlKey: true }), false, false)).toBe("drawer");
    expect(shortcutOf(key("s", { metaKey: true }), true, false)).toBe("save");
    expect(shortcutOf(key("K", { metaKey: true, shiftKey: true }), true, false)).toBe("palette");
    expect(shortcutOf(key("k", { metaKey: true, altKey: true }), true, false)).toBeUndefined();
    expect(isMac("MacIntel")).toBe(true);
    expect(isMac("iPhone")).toBe(true);
    expect(isMac("Linux x86_64")).toBe(false);
    expect(isMac("Win32")).toBe(false);
  });

  it("chords act in a text field, single keys and Escape do not", () => {
    expect(shortcutOf(key("k", { ctrlKey: true }), false, true)).toBe("palette");
    expect(shortcutOf(key("e", { ctrlKey: true }), false, true)).toBe("drawer");
    expect(shortcutOf(key("s", { ctrlKey: true }), false, true)).toBe("save");
    for (const name of ["f", "l", "m", "a", "?", "Escape"]) {
      expect(shortcutOf(key(name, { shiftKey: name === "?" }), false, true)).toBeUndefined();
    }
    expect(shortcutOf(key("Escape"), false, false)).toBe("escape");
  });

  it("single keys need no modifier but shift", () => {
    expect(["f", "l", "m", "a"].map((name) => shortcutOf(key(name), false, false))).toStrictEqual([
      "fit",
      "tidy",
      "minimap",
      "add",
    ]);
    expect(shortcutOf(key("F", { shiftKey: true }), false, false)).toBe("fit");
    expect(shortcutOf(key("f", { ctrlKey: true }), false, false)).toBeUndefined();
    expect(shortcutOf(key("f", { metaKey: true }), true, false)).toBeUndefined();
    expect(shortcutOf(key("f", { altKey: true }), false, false)).toBeUndefined();
    expect(shortcutOf(key("?", { shiftKey: true }), false, false)).toBe("shortcuts");
    expect(shortcutOf(key("Delete"), false, false)).toBeUndefined();
    expect(shortcutOf(key("Backspace"), false, false)).toBeUndefined();
  });

  it("isTextTarget", () => {
    for (const type of [undefined, "", "text", "search", "number"]) {
      expect(isTextTarget({ tagName: "INPUT", type })).toBe(true);
    }
    expect(isTextTarget({ tagName: "TEXTAREA" })).toBe(true);
    expect(isTextTarget({ tagName: "SELECT" })).toBe(true);
    expect(isTextTarget({ tagName: "DIV", isContentEditable: true })).toBe(true);
    for (const type of ["checkbox", "button", "range"]) {
      expect(isTextTarget({ tagName: "INPUT", type })).toBe(false);
    }
    expect(isTextTarget({ tagName: "BUTTON" })).toBe(false);
    expect(isTextTarget(null)).toBe(false);
    // A live control's hidden range input, and anything else inside .nokey, is a text target for the shortcuts.
    const inside = (selector: string) => (selector === ".nokey" ? {} : null);
    expect(isTextTarget({ tagName: "INPUT", type: "range", closest: inside })).toBe(true);
    expect(isTextTarget({ tagName: "DIV", closest: inside })).toBe(true);
    expect(isTextTarget({ tagName: "INPUT", type: "range", closest: () => null })).toBe(false);
  });

  it("labels follow the platform", () => {
    expect(keyLabel("palette", true)).toBe("⌘K");
    expect(keyLabel("palette", false)).toBe("Ctrl+K");
    expect(keyLabel("escape", true)).toBe("Esc");
    expect(rowKeys("delete", false)).toBe("Delete / Backspace");
    for (const row of shortcutRows) {
      for (const mac of [true, false]) expect(rowKeys(row.action, mac)).not.toBe("");
      expect(row.does).not.toBe("");
    }
  });
});
