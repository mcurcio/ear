// The ear.conf drawer's gutter (docs/editor.md#the-drawers-gutter): one row per line the textarea shows, and a dot on
// each line a diagnostic names.
import { describe, expect, it } from "vitest";
import { gutterDigits, gutterLines, lineCount } from "./gutter";
import { shownText } from "./highlight";
import type { Diagnostic } from "./protocol";

const at = (line: number, column: number, severity: string, code = "E1", message = "bad"): Diagnostic =>
  ({ line, column, severity, code, message }) as Diagnostic;

describe("the gutter's lines", () => {
  it("counts the lines the textarea shows", () => {
    expect(lineCount("")).toBe(1);
    expect(lineCount("a")).toBe(1);
    expect(lineCount("a\n")).toBe(2);
    expect(lineCount("a\r\nb\r\n")).toBe(3);
    expect(lineCount("a\rb")).toBe(2);
    for (const text of ["", "a", "a\n", "a\r\nb\r\n", "a\rb", "\r\n\r\n"])
      expect(lineCount(text)).toBe(shownText(text).split("\n").length);
  });

  it("numbers every line and marks only those diagnostics name", () => {
    const lines = gutterLines("one\ntwo\nthree\n", [at(2, 5, "warning", "W1", "odd")]);
    expect(lines.map((line) => line.line)).toEqual([1, 2, 3, 4]);
    expect(lines[0]).toStrictEqual({ line: 1, column: 1, messages: [] });
    expect(lines[1]).toStrictEqual({ line: 2, mark: "warning", column: 5, messages: ["2:5 W1 odd"] });
    expect(lines[2]!.mark).toBeUndefined();
  });

  it("puts error over warning, lists in order and keeps the first diagnostic's column", () => {
    const [line] = gutterLines("node x {", [at(1, 7, "warning", "W1", "first"), at(1, 3, "error", "E2", "second")]);
    expect(line).toStrictEqual({ line: 1, mark: "error", column: 7, messages: ["1:7 W1 first", "1:3 E2 second"] });
    const [after] = gutterLines("x", [at(1, 1, "error"), at(1, 2, "warning")]);
    expect(after!.mark).toBe("error");
  });

  it("marks the last line for a diagnostic past the end, and draws an unknown severity neutrally", () => {
    const lines = gutterLines("a\nb", [at(9, 1, "error"), at(1, 2, "hint")]);
    expect(lines).toHaveLength(2);
    expect(lines[1]!.mark).toBe("error");
    expect(lines[0]!.mark).toBe("other");
    const [both] = gutterLines("a", [at(1, 1, "hint"), at(1, 1, "warning")]);
    expect(both!.mark).toBe("warning");
  });

  it("is as wide as the widest number", () => {
    expect(gutterDigits(1)).toBe(1);
    expect(gutterDigits(9)).toBe(1);
    expect(gutterDigits(10)).toBe(2);
    expect(gutterDigits(120)).toBe(3);
  });
});
